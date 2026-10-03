# jdk add-on

OpenJDK 25 in the sandbox: the JDK's `java` and its tools, built for the guest from the OpenJDK
source (25.0.5+7) — HotSpot with its **Zero** interpreter (no JIT: the guest cannot make code at
run time) and the JDK's native libraries for wasm32, linked statically into one program, with the
class library of the JDK's modules other than the desktop ones.

```
$ java -version
openjdk version "25.0.5" 2026-10-20
OpenJDK Runtime Environment collaboCore-WASM (build 25.0.5+7)
OpenJDK Zero VM collaboCore-WASM (build 25.0.5+7, interpreted mode, static)
```

`collaboCore-WASM` marks it as the WebAssembly port for collaboCore, not an official OpenJDK
build. The add-on installs the JDK as `/usr/lib/jvm/java-25-openjdk` (its `JAVA_HOME`) and adds to
`/usr/bin`:

| Program | |
|---|---|
| `java` | the launcher; also `java Hello.java` (the source launcher) and `java -jar` |
| `javac`, `javadoc`, `javap`, `jdeps`, `jdeprscan`, `jnativescan`, `serialver` | compiler and class-file tools |
| `jar`, `jarsigner`, `jlink`, `jmod`, `jimage` | archives, run-time images |
| `jshell` | the Java shell (snippets run in a second JVM it starts, through JDI, as everywhere) |
| `jcmd`, `jps`, `jstat`, `jstack`, `jinfo`, `jmap`, `jdb` | diagnostics and the debugger (attach API, JDWP) |
| `keytool`, `kinit`, `klist`, `ktab`, `rmiregistry`, `jwebserver` | the rest |

Modules (lib/modules): `java.base`, `java.compiler`, `java.instrument`, `java.logging`,
`java.management(.rmi)`, `java.naming`, `java.net.http`, `java.prefs`, `java.rmi`,
`java.scripting`, `java.security.jgss/sasl`, `java.sql(.rowset)`, `java.transaction.xa`,
`java.xml(.crypto)` and the `jdk.*` modules of the tools above, `jdk.charsets`, `jdk.localedata`
(every locale), `jdk.httpserver`, `jdk.net`, `jdk.zipfs`, `jdk.unsupported` and the like. Not
included: `java.desktop` (AWT, Swing, sound, imaging) and the modules that need it, JFR,
JVMCI/Graal, the serviceability agent, `jpackage`, SCTP, smart cards and PKCS#11.

## Turning it on

```json
"addons": {"jdk": true}
```

or `--addon-dir addons/jdk/out --addon jdk` on the engine's command line. It has no settings.

```sh
cat > Hello.java <<'EOF'
public class Hello { public static void main(String[] a) { System.out.println("Hello " + Runtime.version()); } }
EOF
java Hello.java                      # compiles in memory and runs
javac -d out Hello.java && java -cp out Hello
jar cfe hello.jar Hello -C out . && java -jar hello.jar
printf 'System.out.println(6 * 7)\n/exit\n' > t.jsh && jshell t.jsh
java -cp out Hello & jcmd $! Thread.print
```

Threads, virtual threads, files, `Process` (posix_spawn of `lib/jspawnhelper`), sockets,
`HttpClient` (through the sandbox's network policy, as everything in the guest), TLS, the JDK's
crypto providers, locales and charsets, `java.util.prefs`, JMX, the attach API (`jcmd`) and JDWP
(`jdb`, JDI) all work; `tests/run.sh` checks them in the guest (and the crypto results against
known answers from a desktop JDK).

## Limits

- **Speed.** Zero interprets every bytecode, and the guest runs the interpreter as WebAssembly.
  Measured in the guest (arm64 host): a JVM starts in under a second (one more the first time,
  while the engine compiles `java`); 5 million iterations of a small loop take 1 s, `fib(25)`
  0.08 s; `javac` of a small class 5 s, of a 200-line one 12 s; `javadoc` of a small class 9 s;
  `jshell` 16–20 s to its first result (it starts a second JVM). Crypto runs in C (see below):
  SHA-256 and AES-GCM at tens to hundreds of MB/s, a P-256 signature check in 0.13 s, an
  RSA-2048 signature in 0.14 s; the first HTTPS request of a JVM takes about 5 s (mostly loading
  the classes of TLS), a later one 1–3 s per new server. Long builds (Maven, Gradle) are possible
  but slow.
- **Memory.** One JVM's heap defaults to a quarter of the guest's memory (1 GiB); only what is
  used costs memory. A heap of more than about 900 MB (`-Xmx`) puts the process above 2 GiB of
  guest addresses, which needs an engine with `addons/mod/0007` (in older engines, a guest
  address above 2 GiB in a kernel copy stops the machine).
- **No JNI libraries of your own.** Nothing can be loaded at run time in the guest: the JDK's
  native libraries are linked into `java`, and `System.loadLibrary` finds only those
  (`JNI_OnLoad_<lib>`, as a static JDK does). The FFM API's downcalls and upcalls (the fallback
  linker) are not there either.
- **One garbage collector**, Serial (and Epsilon). No CDS (it maps its archive). No JFR.
- **Headless.** No `java.desktop`: no AWT, Swing, `ImageIO`, `javax.sound`, `java.beans`.
- **Tools that start a second JVM** (`jshell`, `jdb -launch`, test runners) work, at the price
  of a second JVM's start-up. The guest kernel as shipped mixes up the futexes of two processes
  that use the same address (two JVMs do: same program); the JDK works around that where it
  waits itself (its waits look again every 100 ms, at some cost), not in the C library's own
  locks, so two JVMs at once are fully reliable only with `addons/mod/0008`, which fixes the
  kernel.
- **`jlink` needs jmods**: the image has no `jmods/` (and is not a linkable run-time image), so
  `jlink` works only on jmods you bring; `jmod`, `jimage` and `jar` work as is.

## Building

```sh
./build.sh tools userspace          # cmake/ninja, binaryen, musl sysroot, compiler-rt
addons/jdk/build.sh                 # or python3 addons/build.py build jdk
```

The host needs clang-19, lld-19 (`wasm-ld-19`), llvm-ar-19, m4 and perl, network access for the
downloads, and the jdk25u clone at `../org/jdk25u` (beside `CollaboCore`). A first build takes
a few minutes on 6 cores once the downloads are there (the boot JDK, autoconf, LLVM's source for
libc++), and 2 GB of `build/`. Stages, each skipped when up to date (redo one
with `FORCE=stage[,stage]`); the JDK's own make (`java`, `native`, `launcher`) always runs and
knows itself what changed:

| Stage | |
|---|---|
| `tools` | autoconf 2.72 and the boot JDK (Temurin 25.0.4.1 for the build machine), downloaded into `build/tools` |
| `src` | the JDK at `JDK_TAG` (`jdk-25.0.5+7`), exported from the clone into `build/src/jdk` with `patches/jdk/*.patch` applied; the clone itself is never modified |
| `libcxx` | libc++ / libc++abi 19.1.7 for wasm32, static, without exceptions (HotSpot is C++) |
| `deps` | `libwasmcompat.a` (`compat/`), the first `libffi.a` (`ffi/`), and empty cups/fontconfig/alsa headers for configure |
| `configure` | the JDK's configure, cross (`--openjdk-target=wasm32-unknown-linux-musl`): Zero, Serial GC, no CDS/JFR, headless, the boot JDK also as the build JDK, `tools/wasm-jdk-cc` as the compiler; `build/jdk` is its output |
| `java` | `make <module>-java/-copy/-gendata` for the image's modules: the class library, compiled by the boot JDK |
| `libffi` | `libffi.a` again, with a call stub for the signature of every native method of those classes |
| `native` | `make hotspot-zero-static-libs <module>-static-libs`: HotSpot and the modules' native libraries as static libraries |
| `launcher` | `make static-launcher-only`: `java`, linked with all of them (relinked when `libffi.a`, `libwasmcompat.a` or a library changes) |
| `jspawnhelper` | `lib/jspawnhelper`, which `Process` starts with posix_spawn |
| `image` | `jmod create` for each module (target platform `linux-other`: jlink knows no wasm32), `jlink` (compressed `lib/modules`), `bin/java` and the tools as links to it, into `build/image` |
| `cpio` | `out/jdk.cpio`, the overlay, and the licenses in `out/licenses` |

`JOBS=N` sets the make jobs. Logs are in `build/` (`configure.log`, `make-*.log`, `jlink.log`).

## How it works

- **The compiler** is `tools/wasm-jdk-cc` (and `wasm-jdk-c++`): `userspace/bin/wasm-cc` without
  the flags configure gives clang on Linux that mean nothing on wasm32 (`-fPIC`,
  `-fstack-protector`, `-Wl,-z,relro`…), with libc++ for C++, and for programs: the guest's
  link flags, `libwasmcompat.a`, and — when the program has the JVM in it — a table of its
  functions for `dlsym` (`tools/gen-symtab.py`).
- **Static JDK.** The guest cannot load libraries, so the JDK is built the way OpenJDK's static
  JDK is (`STATIC_BUILD`, `JNI_OnLoad_<lib>`): `dlopen(NULL)` is the program itself and `dlsym`
  looks names up in the table — the JNI functions, the JVM's and the libraries' entry points,
  and a few C library functions the JDK asks for by name.
- **One program for every tool.** `bin/javac`, `bin/jar`, … are links to `bin/java`; the
  launcher picks the tool's arguments by the name it is run as (what the JDK otherwise builds
  into a launcher per tool), so `-J`, `@argfiles` and the usage messages behave as usual.
- **`compat/`** is what the guest's musl leaves out: `mmap` and its relatives are emulated on
  memory from `sbrk` (a reservation costs nothing until it is touched; file mappings are copies,
  written back for `MAP_SHARED`), `fork`/`vfork` fail (`Process` uses posix_spawn), the main
  thread's stack for `pthread_getattr_np`, and `dlopen`/`dlsym`/`dladdr`.
- **`ffi/`** is a libffi for Zero's calls into native methods. WebAssembly cannot build a call
  frame at run time, so `ffi_call` goes through a stub generated for each wasm signature: every
  native method of the class library, and every short signature (`tools/gen-ffi-calls.py`).
- **HotSpot** (`patches/jdk/0002`): nothing preempts a thread that runs wasm code, and the guest
  kernel takes its timers and rescheduling on a CPU only when a thread there enters it; Zero
  checks for safepoints on backward branches (it did not) and makes a system call every so
  often, and logging's busy-wait (at VM exit) yields the CPU. The guest kernel has
  no MMU and so keys futexes by address alone, though each process has its own memory: a wake in
  one JVM can take the waiter of another one at the same address, and the thread it was meant
  for sleeps on (every time `jshell` starts its second JVM; a `java` after `jshell` could hang at
  exit). `addons/mod/0008` fixes the kernel; until then untimed waits (`park()`,
  `LockSupport.park()`, `Monitor::wait()`, which may all return spuriously, and the launcher's
  wait for the main thread) look again every 100 ms, so a lost wake-up costs that much instead
  of a hang. No stack guard pages, no 32-bit address-space probing (a reservation would keep the
  memory), a readable page 0, no machine context in signals.
- **Crypto in C** (`patches/jdk/0008`): the methods HotSpot's compilers replace with intrinsics
  for TLS and signatures (AES blocks and AES in counter mode, GHASH, the compression functions of
  MD5, SHA-1, SHA-2 and SHA-512, P-256 Montgomery multiplication, `Math.multiplyHigh`, and
  `BigInteger`'s multiply, square, multiply-add and Montgomery steps) get a Zero method entry
  (`src/hotspot/cpu/zero/zeroIntrinsics_zero.cpp`) that does the same in C, reading the
  arguments off Zero's stack and the fields it needs off the objects; anything unexpected goes
  to the bytecodes. Interpreted, AES-GCM and SHA-256 moved 100–200 KB/s and a P-256 signature
  check took a second; in C they move tens to hundreds of MB/s (`tests/java/CryptoKat.java`
  checks the results against a desktop JDK's). `-XX:-UseZeroCryptoIntrinsics` turns it off.

## Patches

`patches/jdk/`, applied in order to the exported source:

| Patch | |
|---|---|
| `0001-build-wasm32-cpu` | configure knows the `wasm32` CPU (32-bit, little-endian; HotSpot only as Zero) |
| `0002-hotspot-zero-on-the-wasm32-guest` | Zero on the guest: safepoint checks on backward branches and a system call every so often, and a yield in logging's busy-wait (no preemption: the waiter kept its CPU from the thread it waited for); a `switch` for the bytecode dispatch instead of computed gotos (which wasm does not have: a little faster); untimed waits that wake up every 100 ms (lost wake-ups, see above); no stack guard pages; page 0 readable; no probing of the address space; no machine context in signals; `dll_load` knows no architecture |
| `0003-hotspot-zero-frames-without-pc-are-not-empty` | `frame::is_empty()` was true for every Zero interpreter frame (they have no pc), so thread dumps (`jcmd Thread.print`, `Thread.getAllStackTraces`) stopped after the top frame — on any Zero build |
| `0004-launcher-one-static-program-for-java-and-the-tools` | the launcher picks a tool by the name it is run as; a static launcher records its executable's path, which the tools' `-Dapplication.home` needs; its wait for the main thread looks again every 100 ms (lost wake-ups) |
| `0005-libraries-ilp32-statx-and-procfs` | `struct statx` declared with a 32-bit `__uint64_t` on non-glibc ILP32 (every `stat` read zeroes); `libmanagement_ext` no longer includes the unused `<sys/procfs.h>` |
| `0006-security-ec-no-p256-table-at-first-on-an-interpreter` | on an interpreter (`java.vm.info` says `interpreted mode`), the first 64 secp256r1 multiplications of the generator go without its precomputed table: building it costs as much as 64 of them (2 s in the guest), which a TLS server's first handshake would wait for |
| `0007-jimage-read-in-bulk` | `jdk.internal.jimage` reads names and small resources out of `lib/modules` with bulk copies instead of a byte at a time (looking up classes is most of `javac`'s start: it got about a third faster) |
| `0008-hotspot-zero-crypto-in-c` | Zero's method entry for the crypto and big-number intrinsics (`zeroIntrinsics_zero.cpp`, a method kind for it, the `UseZeroCryptoIntrinsics` flag) |

To change one: edit `build/src/jdk` (a git repository whose first commit is the pristine export),
rebuild with `addons/jdk/build.sh`, and write the patches again with `tools/save-patches.sh`
(`git diff` of each patch's files).

## Debugging

- `JDK_WASM_MMAP_TRACE=1`: each `mmap`/`munmap` of the emulation on stderr.
- `JDK_WASM_KEEP_NAMES=1` while linking (`make static-launcher-only`): a `java` that keeps its
  function names, and `JDK_WASM_TRAP_SIGNAL=1` at run time: `SIGSYS` makes the thread that gets
  it trap, and the engine prints its wasm stack with names (send it to one thread with `tgkill`).
- `-XX:-UseZeroCryptoIntrinsics`: crypto and `BigInteger` in Java bytecodes again (to tell
  whether a wrong result comes from `zeroIntrinsics_zero.cpp`).
- `-Xlog:safepoint,vmthread=debug`, `jcmd <pid> Thread.print` and `kill -QUIT` work as usual.

## Licenses

OpenJDK: GPL-2.0 with the Classpath Exception (`LICENSE`, `ASSEMBLY_EXCEPTION`,
`ADDITIONAL_LICENSE_INFO`, and the third-party notices in the image's `legal/`); libc++ and
libc++abi: Apache-2.0 WITH LLVM-exception (`out/licenses/`).
