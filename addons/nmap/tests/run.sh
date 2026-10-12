#!/usr/bin/env bash
# Checks for the nmap add-on, in the guest (test_guest.py): nmap, nping and ncat --version; connect
# and SYN scans of the host's loopback (192.0.2.1) with open and closed ports, the default
# 1000-port scan, host discovery (ARP; an address with no host is down), UDP, version detection,
# NSE scripts over HTTP and TLS, -A; XML and grepable output and --resume; nping; ncat's request
# and half-close, -l with -e, -k -c (concurrent clients), --lua-exec, --ssl and the HTTP proxy.
#
# The guest is the packaged runtime (dist/runtime/collabo-core-<platform>, COLLABO_RUNTIME) with
# out/nmap.cpio (NMAP_CPIO); NMAP_ENGINE and NMAP_KERNEL pick another engine binary or kernel, e.g.
#   NMAP_ENGINE=engine/target/release/collabo-core-engine NMAP_KERNEL=kernel/linux/vmlinux.wasm \
#     addons/nmap/tests/run.sh
# The kernel needs packet sockets (addons/mod/0013), the engine addons/mod/0014 and 0015.
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$T/test_guest.py" "$@"
