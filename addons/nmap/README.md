# nmap add-on

Nmap in the sandbox: the network scanner from nmap.org's development tree (7.991SVN, after
7.991), with **Nping** and **Ncat**, built for the guest — its scan engine, version detection,
OS detection, traceroute, the Nmap Scripting Engine (Lua 5.4) with its 600-odd scripts and
libraries, and its data files, with libpcap 1.10.7, OpenSSL 3.5 and libssh2.

```
$ nmap --version
Nmap version 7.991SVN ( https://nmap.org )
Platform: wasm32-unknown-linux-musl
Compiled with: nmap-liblua-5.4.9 openssl-3.5.7 nmap-libssh2-1.11.1_NMAP1 libz-1.3.1 nmap-libpcre2-10.48 nmap-libpcap-1.10.7 nmap-libdnet-1.18.2 ipv6
Available nsock engines: epoll poll select
```

The add-on adds `nmap` (6.3 MB), `nping` and `ncat` to `/usr/bin`, nmap's data, scripts and NSE
libraries in `/usr/share/nmap` and Ncat's CA bundle in `/usr/share/ncat` (36 MB in all).

Scan only hosts you are allowed to scan. Every probe goes out from this computer, under the
sandbox's network policy, and every connection attempt is a network event the app sees.

## Turning it on

```json
"addons": {"nmap": true}
```

or `--addon-dir addons/nmap/out --addon nmap` on the engine's command line. It has no settings.
The scans that send and receive raw packets (the default SYN scan `-sS`, host discovery, `-sU`,
`-O`, traceroute, `nping`) need a kernel with packet sockets: `CONFIG_PACKET`, which
`addons/mod/0013` turns on (`./build.sh kernel`, then `./build.sh engine runtime`). On a kernel
without them, `nmap -sT -Pn` and `nmap --unprivileged` (connect scans), `-sV`, the scripts and
Ncat still work.

```sh
nmap -sV -p 22,80,443 example.com             # SYN scan, then version detection
nmap -sT -Pn -p- 192.0.2.1                    # every port of this computer (with allowHostLoopback)
nmap --script http-title,ssl-cert -p 443 example.com
nmap -sn scanme.nmap.org                      # is it up? (ICMP echo, TCP SYN)
nping --tcp -p 443 -c 3 example.com
ncat --ssl example.com 443                    # TLS from the terminal; -l, -e, -k, --proxy as usual
```

## How scans look from inside the sandbox

The guest's one link, `eth0`, ends at the engine: the gateway 192.0.2.1 is this computer, and the
engine's own TCP/IP stack carries the guest's traffic to the host's sockets (`engine/src/net.rs`).
nmap's raw packets go out on that link like any others, so what a scan finds is what this
computer finds when it connects, through the policy:

- **TCP SYN and connect scans** are accurate: each SYN becomes a connection from this computer to
  the target port; the guest gets a SYN-ACK when it connects (open), a reset when it is refused
  (closed), and nothing while it waits (filtered: nmap gives up before the engine's 10-second
  connect timeout). A port the **policy** refuses looks closed (the engine resets it).
- **Host discovery**: ICMP echo is a real ping from this computer; TCP SYN probes as above; ACK
  probes and the ACK, FIN, NULL and Xmas scans get no answer from addresses other than the gateway
  (`addons/mod/0015`), as behind a firewall (`filtered`, `open|filtered`): the engine cannot tell.
  Where ICMP is blocked, `-Pn` skips discovery.
- **UDP is not carried**, except DNS: port 53 at any address is answered by the engine's resolver,
  so `-sU` finds 53 open everywhere and the other ports `open|filtered`.
- **Version detection (`-sV`) and the scripts** run over real connections and see the real
  services: banners, HTTP, TLS certificates and ciphers, SSH algorithms and host keys.
- **OS detection (`-O`)** and **traceroute** see the engine's stack one hop away, not the remote
  host: their results say nothing about it.
- The guest's own network is 192.0.2.0/24 with one other host, the gateway (ARP answers it).
  IPv6 is not there.

Each SYN probe is a connection attempt on this computer and a network event the app sees; with
`network.ask`, the app is asked about each host and port an unlisted target is probed on, so list
the targets in `allow` first. Scanning many filtered ports keeps many 10-second connects open at
once on this computer: `-T`, `--max-rate` and `--max-parallelism` keep a large scan gentle.

## Ncat without fork()

The guest has no `fork()`. Ncat's `-e`, `-c` and `--lua-exec` start the command with
`posix_spawn()` (`--lua-exec` runs ncat again with `--lua-exec-internal`, as on Windows); with
`-k`, each connection's command is relayed by a thread of the listener; the HTTP proxy
(`--proxy-type http`) serves each client in a thread. Upstream Ncat's HTTP proxy ends a tunnel when
the client half-closes it, so a client that sends a request and closes its side gets no answer
through it: `ncat --no-shutdown` keeps it open.

## How it is built

`build.sh` exports the clone `../org/nmap` at a pinned commit into `build/src/nmap`, applies
`patches/nmap` and the tcpdump add-on's `patches/libpcap` (to nmap's `libpcap/`), builds libc++
for wasm32 (LLVM 19.1.7's, without exceptions, as the jdk and node add-ons do), and runs nmap's
configure and make with `tools/wasm-nmap-cc` / `wasm-nmap-c++` (the guest's compiler,
`userspace/bin/wasm-cc`, with libc++), static only, with the libraries nmap carries (libpcap,
libdnet, Lua, PCRE2, libssh2, liblinear) and OpenSSL and zlib from `python/deps`. Stages:
`FORCE=src,libcxx,configure,nmap,cpio`.

| Patch | What it does |
|---|---|
| `nmap/0001-mmapfile-without-mmap-on-wasm` | `mmapfile()` reads the file into memory (the guest's libc has no `mmap`), and its `munmap()` writes it back when it was opened for writing: `--resume` |
| `nmap/0002-main-without-cxx-exceptions` | `main()`'s try/catch only when C++ exceptions are there (`__cpp_exceptions`); libc++ here has none |
| `nmap/0003-ncat-exec-and-proxy-without-fork` | Ncat without `fork()`: `posix_spawn()` for `-e`/`-c`/`--lua-exec`, a relaying thread per `-k` connection, a thread per HTTP proxy client (above) |
| `../tcpdump/patches/libpcap/*` | libpcap's capture with `recvmsg()` (no memory-mapped ring), and its version (tcpdump add-on's README) |

## Tests

`tests/run.sh` (`python3 addons/build.py test nmap`) boots guests with the add-on and scans the
host's loopback (192.0.2.1, `--allow-loopback`), where the test serves HTTP and HTTPS: the
versions; `-sT` and `-sS` with an open and a closed port; the default 1000-port scan; `-sn` (the
gateway up by ARP, an address with no host down); `-sU`; `-sV` with `http-title` and `ssl-cert`;
`-A`; `-oX`, `-oG` and `--resume`; `nping --tcp`; Ncat's request and half-close, `-l -e`, `-l -k -c`
with two clients at once, `--lua-exec`, `--ssl` and the HTTP proxy. `COLLABO_RUNTIME`,
`NMAP_ENGINE` and `NMAP_KERNEL` pick the runtime, engine and kernel; the engine needs
`addons/mod/0014` and `0015` for two of the checks.
