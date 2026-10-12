#!/usr/bin/env python3
"""The tcpdump add-on in the guest: tcpdump and its libpcap (recvmsg() capture, patches/libpcap)
on eth0, lo and any — ARP, ICMP, DNS and TCP through the engine's stack, kernel BPF filters,
cooked headers, savefiles written and read back, a capture under load, the statistics on SIGINT.
Run by tests/run.sh.

The guest is the packaged runtime (COLLABO_RUNTIME, default dist/runtime/collabo-core-<platform>)
with out/tcpdump.cpio (TCPDUMP_CPIO); TCPDUMP_ENGINE and TCPDUMP_KERNEL pick another engine binary
or kernel. The kernel needs packet sockets (addons/mod/0013).
"""
import functools
import http.server
import os
import platform
import re
import shutil
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
ENGINE = os.environ.get("TCPDUMP_ENGINE") or os.path.join(RT, "bin/collabo-core-engine")
KERNEL = os.environ.get("TCPDUMP_KERNEL") or os.path.join(RT, "app/images/vmlinux.wasm")
CPIO = os.environ.get("TCPDUMP_CPIO") or os.path.join(A, "out/tcpdump.cpio")

fails = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "\n     " + str(detail)[-3000:]), flush=True)
    if not ok:
        fails.append(name)


def addon_dir():
    d = tempfile.mkdtemp(prefix="tcpdump-addon-")
    os.symlink(os.path.abspath(CPIO), os.path.join(d, "tcpdump.cpio"))
    shutil.copyfile(os.path.join(A, "addon.json"), os.path.join(d, "tcpdump.json"))
    return d


ADDONS = addon_dir()


def guest(work, command, timeout=300):
    """Runs `command` (sh) in a fresh guest with the add-on, in /work; returns (status, output)."""
    argv = [ENGINE, "exec", "--kernel", KERNEL, "--initramfs", f"{RT}/app/images/initramfs.cpio",
            "--addon-dir", ADDONS, "--addon", "tcpdump", "--allow-loopback",
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


def serve(directory):
    """An HTTP server on the host's loopback, for the guest (192.0.2.1 is the host there)."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass
    handler = functools.partial(Quiet, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def stats(out):
    """tcpdump's closing statistics: (captured, received by filter, dropped by kernel)."""
    found = [re.search(r"(\d+) packets? " + what, out) for what in ("captured", "received by filter",
                                                                     "dropped by kernel")]
    return tuple(int(m.group(1)) if m else -1 for m in found)


def main():
    for f in (ENGINE, KERNEL, CPIO):
        if not os.path.exists(f):
            print(f"missing {f}", file=sys.stderr)
            return 2
    work = tempfile.mkdtemp(prefix="tcpdump-test-")
    os.chmod(work, 0o777)
    www = tempfile.mkdtemp(prefix="tcpdump-www-")
    with open(os.path.join(www, "index.html"), "w") as f:
        f.write("<title>tcpdump-check</title>\n")
    with open(os.path.join(www, "big.bin"), "wb") as f:
        f.write(os.urandom(2 * 1024 * 1024))
    port = serve(www)

    rc, out = guest(work, "tcpdump --version")
    check("tcpdump --version", rc == 0 and "tcpdump version" in out and "libpcap version 1.10.7" in out
          and "with recvmsg()" in out, out)

    rc, out = guest(work, "tcpdump -D")
    check("tcpdump -D: eth0, any, lo", rc == 0 and all(f".{d} " in out for d in ("eth0", "any", "lo")), out)

    # eth0: the guest's link to the engine's stack. ARP for the gateway, then ICMP and TCP; a
    # savefile written and read back.
    rc, out = guest(work, f"""
        tcpdump -i eth0 -n -U -w eth0.pcap 2>cap.err & T=$!; sleep 1
        ping -c 2 192.0.2.1 >/dev/null
        nslookup collabo-check.test 192.0.2.1 >/dev/null 2>&1
        wget -q -O page.html http://192.0.2.1:{port}/index.html
        sleep 1; kill -INT $T; wait $T; echo "status $?"; cat cap.err
        tcpdump -n -r eth0.pcap -A 2>&1""")
    check("eth0: the capture ends on SIGINT, with statistics", "status 0" in out and stats(out)[0] > 10
          and stats(out)[2] == 0, out)
    check("eth0: ARP for the gateway and its answer", "Request who-has 192.0.2.1 tell 192.0.2.2" in out
          and "Reply 192.0.2.1 is-at 52:54:00:00:00:01" in out, out)
    check("eth0: ICMP echo and reply", "192.0.2.2 > 192.0.2.1: ICMP echo request" in out
          and "192.0.2.1 > 192.0.2.2: ICMP echo reply" in out, out)
    check("eth0: a DNS question to the gateway", re.search(r"192\.0\.2\.1\.53: \d+\+ A\? collabo-check\.test\.", out)
          is not None, out)
    check("eth0: TCP handshake and the HTTP exchange (-A)", f"192.0.2.1.{port}: Flags [S]" in out
          and f"192.0.2.1.{port} > 192.0.2.2" in out and "GET /index.html HTTP/1.1" in out
          and "<title>tcpdump-check</title>" in out, out)
    check("eth0: savefile read back (EN10MB)", "link-type EN10MB (Ethernet)" in out, out)

    # A kernel filter (BPF on the socket): only that port's segments; -c stops it.
    rc, out = guest(work, f"""
        tcpdump -i eth0 -n -c 4 "tcp port {port}" 2>&1 & T=$!; sleep 1
        ping -c 1 192.0.2.1 >/dev/null; wget -q -O /dev/null http://192.0.2.1:{port}/index.html; wait $T""")
    lines = [line for line in out.splitlines() if re.match(r"\d\d:\d\d:\d\d\.\d+ IP ", line)]
    check("filter \"tcp port N\" -c 4: four segments, no ICMP", len(lines) == 4
          and all(f".{port}" in line for line in lines) and "ICMP" not in out, out)

    # any: cooked capture (SOCK_DGRAM) with direction, Linux cooked v2 with -y
    rc, out = guest(work, f"""
        tcpdump -i any -y LINUX_SLL2 -n -c 3 "tcp port {port}" 2>&1 & T=$!; sleep 1
        wget -q -O /dev/null http://192.0.2.1:{port}/index.html; wait $T""")
    check("any: LINUX_SLL2 with the interface and direction", "link-type LINUX_SLL2" in out
          and re.search(r" eth0\s+Out IP 192\.0\.2\.2\.\d+ > 192\.0\.2\.1\." + str(port), out) is not None
          and re.search(r" eth0\s+In\s+IP 192\.0\.2\.1\." + str(port), out) is not None, out)

    # lo: the guest's own loopback traffic, each segment once (not again as outgoing)
    rc, out = guest(work, """
        tcpdump -i lo -n -U -w lo.pcap "tcp port 5555" 2>/dev/null & T=$!; sleep 1
        (nc -l -p 5555 >/dev/null </dev/null &); sleep 0.3; echo lo-check-payload | nc 127.0.0.1 5555 >/dev/null
        sleep 0.5; kill -INT $T; wait $T; tcpdump -n -A -r lo.pcap 2>&1""")
    check("lo: loopback TCP once each, payload with -A", "lo-check-payload" in out
          and len(re.findall(r"> 127\.0\.0\.1\.5555: Flags \[S\]", out)) == 1, out)

    # Under load: 2 MB through the stack while capturing headers (-s 96) to a file. Every segment
    # is there and nothing is dropped (the socket's 2 MB receive buffer stands in for the ring).
    rc, out = guest(work, f"""
        tcpdump -i eth0 -n -s 96 -w load.pcap "tcp port {port}" 2>cap.err & T=$!; sleep 1
        wget -q -O big.bin http://192.0.2.1:{port}/big.bin; ls -l big.bin
        sleep 1; kill -INT $T; wait $T; cat cap.err
        tcpdump -n -r load.pcap 2>/dev/null | awk '/192.0.2.1.{port} > / {{ n += $NF }} END {{ print "bytes", n }}'""",
                    timeout=600)
    got = re.search(r"bytes (\d+)", out)
    check("load: 2 MB downloaded while capturing", os.path.exists(os.path.join(work, "big.bin"))
          and os.path.getsize(os.path.join(work, "big.bin")) == 2 * 1024 * 1024, out)
    check("load: the capture has every byte of it, none dropped", got is not None
          and int(got.group(1)) >= 2 * 1024 * 1024 and stats(out)[2] == 0, out)

    # Savefiles rotated by time (-G, checked as packets come) and by count (-W): two, then it ends.
    rc, out = guest(work, """
        tcpdump -i eth0 -n -G 1 -W 2 -w rot-%s.pcap icmp 2>&1 & T=$!; sleep 1
        ping -c 4 -i 1 192.0.2.1 >/dev/null; sleep 0.5
        if kill -0 $T 2>/dev/null; then echo "still running"; kill $T; else echo "ended"; fi; ls rot-*.pcap | wc -l""")
    check("-G 1 -W 2: two savefiles, then it ends", "ended" in out
          and re.search(r"^2$", out, re.M) is not None, out)

    print(f"\n{'all passed' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(www, ignore_errors=True)
    shutil.rmtree(ADDONS, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
