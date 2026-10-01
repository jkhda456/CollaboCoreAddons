# llvm add-on

The LLVM toolchain in the sandbox: Clang, the LLVM IR tools, the MC layer, LLD and the
object-file tools, as wasm32 Linux programs (LLVM 24.0.0git, statically linked). Adds to
`/usr/bin`:

| Program | |
|---|---|
| `clang`, `clang++` | C/C++/Objective-C compiler (driver + cc1 in one process) |
| `opt` | LLVM IR optimizer |
| `llc` | LLVM IR → assembly / object (LLVM Target code generators) |
| `llvm-mc` | assembler / disassembler (LLVM MC) |
| `lld`, `ld.lld`, `wasm-ld` | linker: ELF, WebAssembly (and Mach-O / COFF via `lld -flavor`) |
| `llvm-readobj`, `llvm-readelf` | object-file headers, sections, symbols |
| `llvm-objdump` | disassembler / object dumper |
| `llvm-nm` | symbol tables |
| `llvm-dwarfdump` | DWARF debug info |
| `llvm-symbolizer` | address → function, file, line |
| `llvm-objcopy`, `llvm-strip` | copy / transform / strip object files |

Targets: **WebAssembly, X86, AArch64, ARM, RISCV**. The default target triple is
`wasm32-unknown-linux-musl`; give `--target=` (`-mtriple=`, `-triple=`) for the others.

## Turning it on

```json
"addons": {"llvm": true}
```

or `--addon-dir app/images/addons --addon llvm` on the engine's command line. It has no
settings and needs no network.

```sh
cd /tmp
printf 'int sq(int x){return x*x;}\nint _start(void){return sq(7);}\n' > t.c
clang --target=x86_64-linux-gnu -O2 -g -c t.c -o t.o
llvm-objdump -d t.o; llvm-readelf -h t.o; llvm-nm t.o; llvm-dwarfdump --debug-info t.o
ld.lld -e _start t.o -o t.elf
clang --target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -O2 t.c -o t.arm   # driver runs ld.lld
clang --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export=sq -O2 t.c -o t.wasm
clang --target=x86_64-linux-gnu -S -emit-llvm t.c -o t.ll
opt -O2 t.ll -S -o t2.ll && llc t2.ll -o t.s && llvm-mc -triple=x86_64 -filetype=obj t.s -o t3.o
llvm-objcopy --strip-debug t.o t2.o; llvm-symbolizer --obj=t.o 0x0
```

(All of the above was run in the guest.)

## Limits

- **No headers or libraries for any target.** The add-on is the tools only: there is no
  sysroot in the guest, so `#include <stdio.h>` and linking against a libc need one that you
  bring (`--sysroot=`). Freestanding code, objects, IR and assembly work as is.
- **No JIT.** Nothing can be made executable here (no MMU): `lli` and ORC are not included,
  and `Memory::allocateMappedMemory` refuses `MF_EXEC`.
- **Size.** The image is about 400 MB (clang 113 MB, lld 77 MB, opt and llc 69 MB each, the
  rest 5–20 MB), and the kernel unpacks it into memory at boot.

## Building

```sh
./build.sh tools userspace          # cmake/ninja, binaryen, musl sysroot, compiler-rt
addons/llvm/build.sh                # or python3 addons/build.py build llvm
```

The host needs clang-19 and lld-19 (`wasm-ld-19`), and the llvm-project clone at
`../org/llvm-project` (beside `CollaboCore`). Stages, each skipped when up to date (redo one
with `FORCE=stage[,stage]`); logs are `build/<stage>.log`:

| Stage | |
|---|---|
| `native` | `llvm-tblgen`, `clang-tblgen` for the build machine (the cross build runs them) |
| `rt-src` | libc++ / libc++abi sources from `llvmorg-19.1.7`, exported from the clone. libc++ supports only the two newest clang releases, so trunk's cannot be built by clang-19 |
| `libcxx` | libc++abi + libc++ for wasm32, static, **without exceptions** (LLVM, Clang and LLD are built with `-fno-exceptions`, so no unwinder is needed) |
| `src` | `llvm`, `clang`, `lld` (+ `libc`'s shared headers, `libunwind/include`) at `LLVM_REV`, exported from the clone, with `patches/*.patch` applied. The clone itself is never modified |
| `cross` | the tools, cross-compiled with clang-19 using the flags `userspace/bin/wasm-cc` uses (`-matomics -mbulk-memory -mllvm -wasm-enable-sjlj`, the Linux macros, shared memory, table base 3) |
| `cpio` | strip, `wasm-opt --translate-to-exnref` (as `wasm-cc` does), pack `out/llvm.cpio` |

`cross` is the long stage (about 3,600 build steps); links run one at a time to keep memory
down. `JOBS=N` and `NINJA_FLAGS="-k 0"` (keep going, to see every error at once)
are passed to the cross build's ninja.

## Patches

What this platform lacks, in `patches/` (applied in order to the exported source):

| Patch | |
|---|---|
| `0001-support-no-fork-on-wasm` | no `fork()` without an MMU: programs are started with `posix_spawn` only (the clang driver → `ld.lld` works) |
| `0002-support-no-mmap-on-wasm` | no `mmap`: file mappings report "not supported" so files are read instead; "mapped" memory is page-aligned heap memory (lld's output buffers use it) |
| `0003-clang-bytecode-no-musttail-without-wasm-tail-call` | the constant interpreter's `[[clang::musttail]]` needs the wasm tail-call feature; turned off as on PPC/i386 |
| `0004-lld-macho-no-madvise-on-wasm` | lld's Mach-O page-in uses `madvise` |

To change one: edit `build/src/...`, rebuild with `addons/llvm/build.sh` (incremental), then
regenerate the patch against the pinned revision, e.g.

```sh
git -C ../org/llvm-project show $LLVM_REV:llvm/lib/Support/Unix/Path.inc > /tmp/a
diff -u --label a/llvm/lib/Support/Unix/Path.inc --label b/llvm/lib/Support/Unix/Path.inc \
  /tmp/a build/src/llvm/lib/Support/Unix/Path.inc > patches/NNNN-name.patch
touch build/src/.stamp     # the source tree already has it; skip re-exporting
```

## Licenses

LLVM, Clang, LLD, libc++ and libc++abi: Apache-2.0 WITH LLVM-exception
(`out/licenses/`).
