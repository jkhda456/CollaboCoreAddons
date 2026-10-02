# node add-on

`node` in the sandbox: **Node.js v24.21.0** for the guest's WebAssembly Linux, with `npm` and `npx`
(Node's bundled npm), GNU **bash** 5.3 and **ripgrep** 15.2.0 (`rg`). Its purpose is to run
JavaScript programs and npm packages as they are, the **official Claude Code** among them:

```sh
npm install -g @anthropic-ai/claude-code@2.1.112   # the last release that is JavaScript (cli.js);
claude                                             # later ones are native x64/arm64 binaries
```

V8 cannot run on the guest (it generates machine code), so this is Node's own JavaScript library
(`lib/`, unmodified, from the v24.21.0 tag) on the **QuickJS-ng** engine, with libuv and C
implementations of Node's internal bindings (`src/`). Programs see Node 24: `process.version`,
the same modules, errors, events and timing semantics, and `process.arch` is `x64` (programs check
it to pick a code path; nothing native is loaded).

## What works

| Area | |
|---|---|
| Modules | CommonJS, ES modules (static/dynamic `import`, `import.meta`, JSON modules, `require()` of ESM), `node:` builtins, `vm` |
| Files | `fs` (sync, callbacks, promises, streams, `fs.watch`: stat polling in the guest, which has no inotify), `path`, `os` |
| Processes | `child_process` (`spawn`, `exec`, `fork`, sync variants; the guest has no fork(), children start with posix_spawn), signals |
| Network | `net`, `http`, `https`, `http2` (nghttp2: clients, servers, push, the compat API; gRPC works), `tls` (OpenSSL 3.5, Mozilla's roots plus the guest's `/etc/ssl/cert.pem`), `dgram`, `dns` (`lookup`), `fetch`, `WebSocket` (undici) |
| Data | `zlib` (gzip, deflate, brotli, zstd), `crypto` (hashes, HMAC, ciphers, keys, sign/verify, KDFs, ECDH, X.509, `webcrypto`), `Buffer`, streams, `string_decoder`, `structuredClone` |
| Concurrency | `worker_threads` (a QuickJS runtime and event loop per thread; `MessagePort` transfer, `SharedArrayBuffer`, `Atomics.wait`), timers, `AsyncLocalStorage`, `async_hooks` |
| Other | `WebAssembly` (interpreted, wasm3), `readline`, `repl`, `util`, `events`, `assert`, `test` |
| Tools | `npm` and `npx` (install, pack, run scripts; global installs go to `/usr/lib/node_modules`, their commands to `/usr/bin`), `bash`, `rg` |

`Intl` is this add-on's own (`src/intl.c`, `src/intl.js`, data from Unicode 17 and CLDR 48):
`Segmenter` (grapheme, word, sentence: Unicode's rules, as V8 has them), `NumberFormat` (every
locale), `DateTimeFormat` with the IANA time zones, `Collator`, `PluralRules`,
`RelativeTimeFormat`, `ListFormat`, `DisplayNames`, `Locale`. English output matches V8; other
locales get their own numbers, but dates and other texts in en-US form, word segmentation has no
dictionaries (Chinese, Japanese, Thai), collation has no locale tailorings, and only Latin digits
and the Gregorian calendar are supported.

Not available: the inspector and debugger protocol, heap snapshots and V8 flags (accepted and
ignored), native addons (`.node` files).

Claude Code finds bash for its Bash tool and, in the guest, uses `rg` from `PATH` for Grep and Glob
(node sets `USE_BUILTIN_RIPGREP=0` there: the ripgrep it bundles is a native binary).

## Start-up: the compile cache

QuickJS has no JIT and parses each program's source when it starts: Claude Code's `cli.js` (13 MB)
takes about 3.5 s to parse in the guest. node keeps the compiled bytecode on disk, keyed by the
source (Node's compile cache, `src/compile_cache.c`): `NODE_COMPILE_CACHE=dir` or
`module.enableCompileCache()` turn it on as in Node, and in the guest it is on by itself for files
of 32 KB and more, in `/tmp/node-compile-cache`; Claude Code then starts in about 0.4 s, from its
second start after each boot (`NODE_COMPILE_CACHE=/work/.node-cache`, a folder of the host's, keeps
it between boots; `NODE_DISABLE_COMPILE_CACHE=1` turns it off). `NODE_DEBUG_NATIVE=COMPILE_CACHE`
reports hits and misses.

## The engine

Node's code recurses deeper than the default WebAssembly stack of the engine (512 KiB per guest
thread) allows: npm, for one, fails with "Maximum call stack size exceeded". With
`addons/mod/0006-engine-room-for-deep-recursion-in-guest-programs.patch` the engine gives guest
threads 32 MiB and tells the guest so (`collabo.wasmstack=` on the kernel command line); node
then allows 12 MiB of JavaScript stack, about 13,000 calls. Without it node stays within half of
the default stack (so a deep recursion is a `RangeError`, never a trap that stops the machine).
`NODE_STACK_SIZE` (KiB) or `--stack-size=` sets the limit.

## Settings from the app

The engine writes the add-on's settings to `/etc/collabo/addons/node.json`; node reads them when it
starts, as defaults for its environment (and that of what it runs):

| Key | Meaning |
|---|---|
| `env` | an object of environment variables (or that object as a JSON string, as `--addon-config` gives it) |
| `provider` | `anthropic` (default) or `openai`: which variables `baseUrl` and `apiKey` set |
| `baseUrl` | `ANTHROPIC_BASE_URL` (or `OPENAI_BASE_URL`) |
| `apiKey` | becomes a host-side secret for `baseUrl`'s host: the host adds it to the guest's HTTPS requests there (`x-api-key`, or `authorization: Bearer` for openai), replacing what the guest sends, and node sets `ANTHROPIC_API_KEY` (`OPENAI_API_KEY`) to a placeholder so that clients send one. A plain-http `baseUrl` (a server on this computer) gets the key itself in the guest. |

So with `"addons": {"node": {"apiKey": "sk-ant-…"}}`, the official Claude Code talks to the
Anthropic API without ever seeing the key. (The first time, its terminal UI asks whether to use
the key from the environment.) The guest needs the network policy to allow `api.anthropic.com`,
and `registry.npmjs.org` to install packages.

## Building

```sh
addons/node/build.sh            # writes out/node.cpio and out/licenses/
TEST=1 addons/node/build.sh     # also build/host/bin/node, a native build for the tests
addons/node/tests/run.sh        # node_cases.js natively, then the guest checks
```

`build.sh` stages (`FORCE=stage,…` redoes them): `src` (Node's `lib/` and C dependencies from
`../org/node` at v24.21.0, QuickJS-ng at 2f0aa72 from `../org/quickjs-ng`, with `patches/`),
`wasm3`, `libcxx` (LLVM 19.1.7's libc++, for ada; from `../org/llvm-project` or the release
archive), `node` (`make TARGET=wasm`), `bash`, `ripgrep`, `host` and `cpio`. It needs the
`userspace` step (musl, compiler-rt) and the `python` step (OpenSSL, zlib for the guest). Node's
own C dependencies come from its tree: libuv, llhttp, nghttp2, ada, brotli, zstd.

The tests use the official Claude Code 2.1.112 from the npm registry (downloaded into
`build/test`, checked against the registry's sha512; it is not part of the add-on) against a
scripted Anthropic API (`tests/mock_anthropic.py`).

## Layout

```
src/                C: the program (main.c), the environment (env.c), Node's internal bindings
                    (b_*.c, crypto*.c, streams.c, worker.c, ...)
patches/quickjs/    QuickJS-ng changes for Node: host hooks (modules, errors, promise hooks,
                    WeakRef jobs), stack checks on wasm, process-wide class ids
patches/node/       libuv: posix_spawn only, no io_uring, a thread entry for wasm
patches/ripgrep/    no mmap, no fork on wasm
build/patchwork/    the scripts the patches were made with
tools/              fetch/build scripts for wasm3, bash, ripgrep; generators for tables
tests/              node_cases.js, the guest tests, the mock API
```
