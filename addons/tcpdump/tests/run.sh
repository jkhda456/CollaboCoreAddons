#!/usr/bin/env bash
# Checks for the tcpdump add-on, in the guest (test_guest.py): tcpdump --version and -D; captures on
# eth0 (ARP, ICMP, DNS, TCP and HTTP through the engine's stack, written to a savefile and read
# back), with a kernel filter and -c, on any (cooked, with direction) and on lo; 2 MB under capture
# with nothing dropped; the statistics on SIGINT; savefiles rotated on an idle link (-G).
#
# The guest is the packaged runtime (dist/runtime/collabo-core-<platform>, COLLABO_RUNTIME) with
# out/tcpdump.cpio (TCPDUMP_CPIO); TCPDUMP_ENGINE and TCPDUMP_KERNEL pick another engine binary or
# kernel, e.g.
#   TCPDUMP_KERNEL=kernel/linux/vmlinux.wasm addons/tcpdump/tests/run.sh
# The kernel needs packet sockets (addons/mod/0013).
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$T/test_guest.py" "$@"
