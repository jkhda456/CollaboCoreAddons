# tcpdump add-on

tcpdump in the sandbox: the packet analyzer from tcpdump.org's development tree (5.0.0-PRE, after
4.99.6) and libpcap 1.10.7, built for the guest as one static program. It captures on the guest's
network devices — `eth0`, the guest's link to the engine's network stack, `lo` and `any` — with
BPF filters run in the kernel, decodes what it captures, and writes and reads pcap files.

```
$ tcpdump --version
tcpdump version 5.0.0-PRE-GIT
libpcap version 1.10.7 (64-bit time_t, with recvmsg(), without memory-mapped capture)
32-bit build, 64-bit time_t
```

The add-on installs `/usr/bin/tcpdump` (1.4 MB).

## Turning it on

```json
"addons": {"tcpdump": true}
```

or `--addon-dir addons/tcpdump/out --addon tcpdump` on the engine's command line. It has no
settings. It needs a kernel with packet sockets: `CONFIG_PACKET`, which `addons/mod/0013` turns on
(`./build.sh kernel` and then `./build.sh engine runtime` put it in `dist/`). On a kernel without
them tcpdump says `PF_PACKET sockets not supported - the kernel needs CONFIG_PACKET`.

```sh
tcpdump -D                                      # eth0, any, lo
tcpdump -i eth0 -n                              # everything on the guest's link, until Ctrl-C
tcpdump -i eth0 -n -A 'tcp port 80'             # with the payload as text
tcpdump -i any -n -c 20 'host 192.0.2.1'        # every device, with the direction (In/Out)
tcpdump -i eth0 -w /work/capture.pcap           # a file Wireshark opens; -r reads one back
```

## What it sees

The guest has one Ethernet link, `eth0` (192.0.2.2), whose other end is the engine: the gateway
192.0.2.1 is this computer, and the engine's own small TCP/IP stack carries the guest's traffic to
the host's sockets under the network policy (`engine/src/net.rs`). A capture on `eth0` shows the
guest's real frames on that link, as the guest's kernel sends and receives them:

- ARP for the gateway and its answer (`52:54:00:00:00:01`);
- ICMP echo to any address, answered with the real host's echo when the host answers (`ping`);
- DNS questions to the gateway (or any address on port 53), answered by the engine with the
  host's resolver, under the policy (A records only: AAAA has no answer);
- TCP: every connection is the guest's own, from its SYN to its last FIN; the other side is the
  engine, which connects to the real host and relays (a refused connection is a reset; a
  connection the policy refuses is a reset too). Window sizes and options on the far side are the
  engine's, not the remote host's (the engine sends no TCP options, an MSS of 1400 bytes).

Other UDP is not carried, and the guest has no IPv6. `lo` shows the guest's own loopback traffic
(servers and clients inside the guest), `any` both, in Linux cooked headers.

## How it is built

`build.sh` exports the clones `../org/tcpdump` and `../org/nmap` (whose `libpcap/` is libpcap
1.10.7 as tcpdump.org released it, with nmap's trimmed build) at pinned commits into `build/src`,
applies `patches/`, builds `libpcap.a` with the guest's C compiler (`userspace/bin/wasm-cc`) and
tcpdump with CMake, and packs `out/tcpdump.cpio`. The nmap add-on links the same libpcap with the
same patches.

| Patch | What it does |
|---|---|
| `libpcap/0001-linux-capture-with-recvmsg-without-mmap` | libpcap 1.10 reads packets only from the memory-mapped ring (TPACKET_V2/V3), which a guest program cannot map (a WebAssembly process's memory is its own; the guest's libc has no `mmap`). On `__wasm__` it reads one packet at a time with `recvmsg()`, into a buffer laid out as a ring frame, and hands it to the code that handles the ring's (cooked headers, VLAN tags from `PACKET_AUXDATA`, direction, filtering in userland, snapshot length); kernel timestamps from `SO_TIMESTAMPNS`; a 2 MB socket receive buffer stands in for the ring; a timeout makes the read return, as `recvfrom()` did before 1.10 |
| `libpcap/0002-configure-the-package-name-and-version` | nmap's copy of libpcap renamed `VERSION` and its generated configure lost the name and version: `libpcap version 1.10.7` and a working `libpcap.pc` again |

Not built in: libcrypto (no ESP decryption, `-E`; no TCP-MD5 checks), libsmi (no SNMP MIB names),
libcap-ng; `-z` (a command run on each finished savefile) needs `fork()`, which the guest lacks.

## Tests

`tests/run.sh` (`python3 addons/build.py test tcpdump`) boots guests with the add-on: `--version`,
`-D`; a capture on `eth0` of ARP, ICMP, a DNS question and an HTTP exchange with a server on the
host, written to a savefile and read back with `-A`; a kernel filter with `-c`; `any` with
`LINUX_SLL2` and directions; `lo`, each segment once; 2 MB downloaded under capture with every byte
in the file and nothing dropped; the statistics on SIGINT; savefiles rotated with `-G`/`-W`.
`COLLABO_RUNTIME`, `TCPDUMP_ENGINE` and `TCPDUMP_KERNEL` pick the runtime, engine and kernel.
