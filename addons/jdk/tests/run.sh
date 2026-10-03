#!/usr/bin/env bash
# Checks for the jdk add-on, in the guest (test_guest.py): java -version; javac, java and a set of
# checks of the class library and the VM (java/Checks.java: collections, arithmetic, exceptions,
# stack overflow, threads, virtual threads, thread dumps, files, zip, Process, sockets, HttpClient
# and HTTPS (TLS 1.3, a self-signed certificate imported with keytool) to the host, Korean locale
# and charsets, crypto, management, a busy thread beside sleep and GC);
# the source launcher; jar and javap; jshell; jcmd.
#
# The guest is the packaged runtime (dist/runtime/collabo-core-<platform>, COLLABO_RUNTIME) with
# out/jdk.cpio (JDK_CPIO); JDK_ENGINE picks another engine binary, e.g.
#   JDK_ENGINE=engine/target/release/collabo-core-engine addons/jdk/tests/run.sh
# JDK_TEST_BIG_HEAP=1 adds a JVM with a 2 GiB heap (needs an engine with addons/mod/0007).
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$T/test_guest.py" "$@"
