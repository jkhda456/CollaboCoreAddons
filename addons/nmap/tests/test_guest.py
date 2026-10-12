#!/usr/bin/env python3
"""The nmap add-on in the guest: nmap's scans through the engine's stack (connect and raw SYN scans,
host discovery, UDP, version detection, NSE scripts over HTTP and TLS, -A), its output formats and
--resume (patches/nmap/0001), Nping, and Ncat (connect, listen with -e/-c/-k, --lua-exec, --ssl,
the HTTP proxy: patches/nmap/0003). Run by tests/run.sh.

The guest is the packaged runtime (COLLABO_RUNTIME, default dist/runtime/collabo-core-<platform>)
with out/nmap.cpio (NMAP_CPIO); NMAP_ENGINE and NMAP_KERNEL pick another engine binary or kernel.
The raw-packet checks need a kernel with packet sockets (addons/mod/0013); two checks need the
engine's TCP fixes (addons/mod/0014: a half-closed request, 0015: an address with no host is down).
"""
import functools
import http.server
import os
import platform
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

T = os.path.dirname(os.path.abspath(__file__))
A = os.path.dirname(T)
ROOT = os.path.dirname(os.path.dirname(A))
PLATFORM = {("Linux", "x86_64"): "linux-x64", ("Linux", "aarch64"): "linux-arm64",
            ("Darwin", "arm64"): "darwin-arm64", ("Darwin", "x86_64"): "darwin-x64"}.get(
                (platform.system(), platform.machine()), "linux-x64")
RT = os.environ.get("COLLABO_RUNTIME") or os.path.join(ROOT, "dist/runtime/collabo-core-" + PLATFORM)
ENGINE = os.environ.get("NMAP_ENGINE") or os.path.join(RT, "bin/collabo-core-engine")
KERNEL = os.environ.get("NMAP_KERNEL") or os.path.join(RT, "app/images/vmlinux.wasm")
CPIO = os.environ.get("NMAP_CPIO") or os.path.join(A, "out/nmap.cpio")

fails = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "\n     " + str(detail)[-3000:]), flush=True)
    if not ok:
        fails.append(name)


def addon_dir():
    d = tempfile.mkdtemp(prefix="nmap-addon-")
    os.symlink(os.path.abspath(CPIO), os.path.join(d, "nmap.cpio"))
    shutil.copyfile(os.path.join(A, "addon.json"), os.path.join(d, "nmap.json"))
    return d


ADDONS = addon_dir()


def guest(work, command, timeout=600):
    """Runs `command` (sh) in a fresh guest with the add-on, in /work; returns (status, output)."""
    argv = [ENGINE, "exec", "--kernel", KERNEL, "--initramfs", f"{RT}/app/images/initramfs.cpio",
            "--addon-dir", ADDONS, "--addon", "nmap", "--allow-loopback",
            "--mount", f"{work}:/work", "--cwd", "/work", "--", "/bin/sh", "-c", command]
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return -1, f"timed out after {timeout} s: {e.stdout or ''}"
    # the command's output is stdout; stderr is the kernel's boot messages, then the command's
    err = p.stderr
    mark = err.find("init: starting /bin/sh")
    if mark >= 0:
        err = err[err.find("\n", mark) + 1:]
    return p.returncode, p.stdout + err


def serve(directory, tls=None):
    """An HTTP(S) server on the host's loopback, for the guest (192.0.2.1 is the host there)."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass
    class Server(http.server.ThreadingHTTPServer):
        def handle_error(self, request, client_address):
            pass  # the scans' probes hang up mid-request
    handler = functools.partial(Quiet, directory=directory)
    server = Server(("127.0.0.1", 0), handler)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(*tls)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def certificate(directory):
    """A self-signed P-256 certificate for 192.0.2.1 (the host, seen from the guest)."""
    cert, key = os.path.join(directory, "cert.pem"), os.path.join(directory, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                    "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=nmap-check.test",
                    "-addext", "subjectAltName=IP:192.0.2.1"], check=True, capture_output=True)
    return cert, key


def free_port():
    """A loopback port with nothing listening on it (closed, for the scans)."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    for f in (ENGINE, KERNEL, CPIO):
        if not os.path.exists(f):
            print(f"missing {f}", file=sys.stderr)
            return 2
    work = tempfile.mkdtemp(prefix="nmap-test-")
    os.chmod(work, 0o777)
    www = tempfile.mkdtemp(prefix="nmap-www-")
    with open(os.path.join(www, "index.html"), "w") as f:
        f.write("<html><head><title>nmap-check page</title></head><body>ok</body></html>\n")
    http_port = serve(www)
    tls_port = serve(www, certificate(tempfile.mkdtemp(prefix="nmap-tls-")))
    closed = free_port()
    ports = f"{http_port},{closed}"

    rc, out = guest(work, "nmap --version; nping --version; ncat --version")
    check("nmap --version", rc == 0 and "Nmap version 7.99" in out and "nmap-libpcap-1.10.7" in out
          and "openssl-3" in out and "nmap-libssh2" in out and "nmap-liblua-5.4" in out, out)
    check("nping and ncat --version", "Nping version 7.99" in out and "Ncat: Version 7.99" in out, out)

    # Connect scan (the guest's TCP) and SYN scan (raw packets out through a packet socket, the
    # answers through libpcap): the gateway is the host's loopback, where one port listens.
    for kind in ("-sT", "-sS"):
        rc, out = guest(work, f"nmap {kind} -Pn -n --reason -p {ports} 192.0.2.1")
        check(f"{kind}: open and closed ports",
              re.search(rf"^{http_port}/tcp\s+open\s", out, re.M) is not None
              and re.search(rf"^{closed}/tcp\s+closed\s", out, re.M) is not None, out)

    # The default scan: SYN, the 1000 most common ports
    rc, out = guest(work, "nmap -n 192.0.2.1")
    check("nmap 192.0.2.1 (the 1000 ports)", re.search(r"Not shown: 9\d\d closed tcp ports \(reset\)", out)
          is not None and "1 host up" in out, out)

    # Host discovery: the gateway answers ARP; an address with no host, probed with a TCP ACK
    # (which opens no connection), gets no answer (addons/mod/0015).
    rc, out = guest(work, "nmap -sn -n --reason 192.0.2.1; nmap -sn -n -PA80 10.255.255.1")
    check("-sn: the gateway is up (ARP)", "Host is up, received arp-response" in out, out)
    check("-sn -PA80: an address with no host is down", "(0 hosts up)" in out, out)

    # UDP: DNS is answered (by the engine, for any address); other UDP is not carried
    rc, out = guest(work, "nmap -sU -Pn -n -p 53,123 192.0.2.1")
    check("-sU: 53 open (DNS), 123 open|filtered", re.search(r"^53/udp\s+open\s", out, re.M) is not None
          and re.search(r"^123/udp\s+open\|filtered\s", out, re.M) is not None, out)

    # Version detection and NSE scripts (Lua, nsock, OpenSSL)
    rc, out = guest(work, f"nmap -sV -Pn -n -p {http_port},{tls_port} --script http-title,ssl-cert 192.0.2.1")
    check("-sV: the HTTP server's product", re.search(rf"^{http_port}/tcp\s+open\s+http\s+SimpleHTTPServer", out, re.M)
          is not None, out)
    check("-sV: TLS detected (ssl/http)", re.search(rf"^{tls_port}/tcp\s+open\s+ssl/http", out, re.M)
          is not None, out)
    check("NSE http-title", "|_http-title: nmap-check page" in out, out)
    check("NSE ssl-cert", "ssl-cert: Subject: commonName=nmap-check.test" in out
          and "IP Address:192.0.2.1" in out, out)

    # -A: OS detection, version detection, scripts, traceroute
    rc, out = guest(work, f"nmap -A -n -p {http_port},{closed} 192.0.2.1")
    check("-A: OS detection, versions, scripts and traceroute", rc == 0
          and "OS and Service detection performed" in out and "TRACEROUTE" in out
          and "http-title: nmap-check page" in out, out)

    # Output formats; --resume reads the log back (mmapfile without mmap: patches/nmap/0001)
    rc, out = guest(work, f"""
        nmap -sT -Pn -n -p {http_port} -oX scan.xml -oG all.gnmap 192.0.2.1 127.0.0.1 >/dev/null
        grep -c '<port protocol="tcp" portid="{http_port}">' scan.xml
        grep -v 'Host: 127.0.0.1\\|# Nmap done' all.gnmap | head -c -1 > part.gnmap
        nmap --resume part.gnmap | grep -c "Nmap scan report for 127.0.0.1"
        tail -c 1 part.gnmap | od -An -c""")
    check("-oX and -oG", re.search(r"^2$", out, re.M) is not None, out)
    check("--resume: the hosts left, and the log ends with a newline", re.search(r"^1$", out, re.M) is not None
          and re.search(r"^\s*\\n\s*$", out, re.M) is not None, out)

    # Nping: raw TCP probes, their answers through libpcap
    rc, out = guest(work, f"nping --tcp -p {http_port} -c 2 192.0.2.1")
    check("nping --tcp: two SYN-ACKs", re.search(r"Rcvd: 2 ", out) is not None and " SA " in out, out)

    # Ncat: a request and half-close (the engine's FIN no longer overtakes it: addons/mod/0014)
    rc, out = guest(work, f"printf 'GET /index.html HTTP/1.0\\r\\n\\r\\n' | ncat 192.0.2.1 {http_port}")
    check("ncat: a request, then half-close", "HTTP/1.0 200 OK" in out and "nmap-check page" in out, out)

    # Ncat in listen mode, without fork() (patches/nmap/0003): -e, -k -c (concurrent clients, each
    # command in a thread's care), --lua-exec (ncat again), --ssl, the HTTP proxy (a thread each)
    rc, out = guest(work, """
        ncat -l 7000 -e /bin/cat & sleep 1; echo exec-echo | ncat 127.0.0.1 7000; wait
        ncat -l -k 7001 -c 'echo "from $NCAT_REMOTE_ADDR"; cat' & K=$!; sleep 1
        (echo keep-one | ncat 127.0.0.1 7001 > one.txt) & (echo keep-two | ncat 127.0.0.1 7001 > two.txt) &
        wait %2 %3 2>/dev/null; sleep 0.5; cat one.txt two.txt; kill $K
        printf 'print("lua-said " .. io.read())\\n' > t.lua; ncat -l 7002 --lua-exec t.lua & sleep 1
        echo hi | ncat 127.0.0.1 7002; wait
        ncat --ssl -l 7003 -c 'echo tls-secret' & sleep 3; ncat --ssl 127.0.0.1 7003 </dev/null; wait""")
    check("ncat -l -e /bin/cat", "exec-echo" in out, out)
    check("ncat -l -k -c: two clients at once, NCAT_REMOTE_ADDR", "keep-one" in out and "keep-two" in out
          and out.count("from 127.0.0.1") == 2, out)
    check("ncat --lua-exec", "lua-said hi" in out, out)
    check("ncat --ssl (a certificate made at start)", "tls-secret" in out, out)

    rc, out = guest(work, f"""
        ncat -l -k 7004 --proxy-type http & P=$!; sleep 1
        for i in 1 2; do printf 'GET /index.html HTTP/1.0\\r\\n\\r\\n' |
          ncat --no-shutdown --proxy 127.0.0.1:7004 --proxy-type http 192.0.2.1 {http_port} | head -1; done
        kill $P""")
    check("ncat --proxy-type http: two clients through it", out.count("HTTP/1.0 200 OK") == 2, out)

    print(f"\n{'all passed' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(www, ignore_errors=True)
    shutil.rmtree(ADDONS, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
