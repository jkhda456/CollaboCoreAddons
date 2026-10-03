#!/usr/bin/env python3
"""The jdk add-on in the guest: java, javac, jar, javap, jshell, jcmd, a set of checks of the
class library and the VM (tests/java/Checks.java) and known answers of the crypto and big-number
methods Zero runs in C (tests/java/CryptoKat.java, tests/crypto-kat.txt). Run by tests/run.sh.

The guest is the packaged runtime (COLLABO_RUNTIME, default dist/runtime/collabo-core-<platform>)
with out/jdk.cpio (JDK_CPIO); JDK_ENGINE picks another engine binary. JDK_TEST_BIG_HEAP=1 also
runs a JVM with a 2 GiB heap, which needs an engine with addons/mod/0007 (guest addresses of
2 GiB and up).
"""
import functools
import http.server
import os
import platform
import shutil
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
ENGINE = os.environ.get("JDK_ENGINE") or os.path.join(RT, "bin/collabo-core-engine")
CPIO = os.environ.get("JDK_CPIO") or os.path.join(A, "out/jdk.cpio")

fails = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "\n     " + str(detail)[-3000:]), flush=True)
    if not ok:
        fails.append(name)


def addon_dir():
    d = tempfile.mkdtemp(prefix="jdk-addon-")
    os.symlink(CPIO, os.path.join(d, "jdk.cpio"))
    shutil.copyfile(os.path.join(A, "addon.json"), os.path.join(d, "jdk.json"))
    return d


ADDONS = addon_dir()


def guest(work, command, timeout=900):
    """Runs `command` (sh) in a fresh guest with the add-on, in /work; returns (status, output)."""
    argv = [ENGINE, "exec", "--kernel", f"{RT}/app/images/vmlinux.wasm", "--initramfs",
            f"{RT}/app/images/initramfs.cpio", "--addon-dir", ADDONS, "--addon", "jdk", "--allow-loopback",
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
    handler = functools.partial(Quiet, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
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
                    "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=192.0.2.1",
                    "-addext", "subjectAltName=IP:192.0.2.1"], check=True, capture_output=True)
    return cert, key


def main():
    for f in (ENGINE, f"{RT}/app/images/vmlinux.wasm", CPIO):
        if not os.path.exists(f):
            print(f"missing {f}", file=sys.stderr)
            return 2
    work = tempfile.mkdtemp(prefix="jdk-test-")
    os.chmod(work, 0o777)
    for f in os.listdir(os.path.join(T, "java")):
        shutil.copy(os.path.join(T, "java", f), work)
    shutil.copy(os.path.join(T, "jshell.jsh"), work)
    www = tempfile.mkdtemp(prefix="jdk-www-")
    with open(os.path.join(www, "check.txt"), "w") as f:
        f.write("jdk-check\n")
    port = serve(www)
    tls = certificate(work)
    tls_port = serve(www, tls)

    rc, out = guest(work, "java -version")
    check("java -version", rc == 0 and "collaboCore-WASM" in out and "Zero VM" in out, out)

    rc, out = guest(work, "javac -d out Hello.java Checks.java Sleeper.java CryptoKat.java && java -cp out Hello && "
                          "keytool -importcert -noprompt -alias test -file cert.pem -keystore trust.p12 "
                          "-storepass changeit >/dev/null && "
                          "java -Djavax.net.ssl.trustStore=trust.p12 -Djavax.net.ssl.trustStorePassword=changeit "
                          f"-cp out Checks http://192.0.2.1:{port}/check.txt https://192.0.2.1:{tls_port}/check.txt")
    check("javac in the guest", rc == 0 and os.path.exists(os.path.join(work, "out/Checks.class")), out)
    check("java Hello", "Hello from Java 25" in out and "Linux/wasm32" in out, out)
    for line in out.splitlines():
        if line.startswith(("OK   ", "FAIL ")):
            name = line[5:].split(":")[0]
            check("Checks: " + name, line.startswith("OK"), line)
    check("Checks: all", "ALL OK" in out, out)

    rc, out = guest(work, "java -cp out CryptoKat kat.txt")
    with open(os.path.join(T, "crypto-kat.txt")) as f:
        expected = f.read()
    try:
        with open(os.path.join(work, "kat.txt")) as f:
            got = f.read()
    except OSError:
        got = ""
    wrong = [line for line in got.splitlines() if line not in expected.splitlines()]
    check("crypto known answers (Zero's C methods, patch 0008)", rc == 0 and got == expected,
          out + "\n".join(wrong[:10]) + f"\n{len(got.splitlines())} lines, {len(wrong)} unexpected")

    rc, out = guest(work, "java Hello.java")
    check("java Hello.java (source launcher)", rc == 0 and "Hello from Java" in out, out)

    rc, out = guest(work, "jar cfe hello.jar Hello -C out Hello.class && jar tf hello.jar && java -jar hello.jar "
                          "&& javap -cp out Hello")
    check("jar, java -jar, javap", rc == 0 and "META-INF/MANIFEST.MF" in out and "Hello from Java" in out
          and "public class Hello" in out, out)

    # then a JVM that runs a few seconds while jshell's agent JVM exits: on the guest kernel as
    # shipped, the two JVMs' futexes at the same addresses mix (addons/mod/0008), and its launcher
    # waited forever for its main thread
    rc, out = guest(work, "jshell jshell.jsh && java -cp out Sleeper 3000 && echo after-jshell-done", timeout=300)
    check("jshell (a script; remote execution through JDI)", "x=42" in out and "JSHELL" in out, out)
    check("a JVM right after jshell exits", rc == 0 and "after-jshell-done" in out, out)

    rc, out = guest(work, "java -cp out Sleeper 60000 & sleep 3; jcmd $! Thread.print; jcmd $! VM.version; "
                          "kill $!")
    check("jcmd Thread.print, VM.version", "Thread.sleep" in out and "Sleeper.main" in out
          and "OpenJDK Zero VM" in out, out)

    if os.environ.get("JDK_TEST_BIG_HEAP"):
        rc, out = guest(work, "java -Xmx2g -Xms1600m -cp out Hello | head -1; echo rc=$?")
        check("2 GiB heap", "Hello from Java" in out and "rc=0" in out, out)

    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(www, ignore_errors=True)
    shutil.rmtree(ADDONS, ignore_errors=True)
    print(f"{'all passed' if not fails else str(len(fails)) + ' failed: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
