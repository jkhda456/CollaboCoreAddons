#!/usr/bin/env bash
# Build the llvm add-on: LLVM toolchain tools as wasm32 Linux programs, packed as
# out/llvm.cpio (an overlay: /usr/bin/{clang,opt,llc,llvm-mc,ld.lld,llvm-readobj,
# llvm-objdump,llvm-nm,llvm-dwarfdump,llvm-symbolizer,llvm-objcopy} and symlinks).
#
# Stages (each skipped when its output is up to date):
#   native   llvm-tblgen and clang-tblgen for the build machine
#   rt-src   libc++/libc++abi sources of LLVM 19.1.7 (the host clang-19's release)
#   libcxx   libc++abi + libc++ for wasm32, without exceptions
#   src      LLVM/Clang/LLD at LLVM_REV, with patches/*.patch applied
#   cross    the tools, cross-compiled for wasm32 (Linux, musl)
#   cpio     strip, translate the SJLJ encoding, pack out/llvm.cpio
#
# Needs: ./build.sh tools userspace (cmake/ninja, binaryen, sysroot, compiler-rt), clang-19 and
# lld-19 on the host, and the llvm-project clone at ../org/llvm-project.
#
#   FORCE=native,rt-src,libcxx,src,cross,cpio  re-run the named stages
#   JOBS=N, NINJA_FLAGS="-k 0"                  passed to the cross build's ninja
set -euo pipefail

A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
LLVM_SRC="$ROOT/../org/llvm-project"
U="$ROOT/userspace"
TOOLS="$ROOT/.tools"
B="$A/build"
OUT="$A/out"

CMAKE="$TOOLS/cmake/bin/cmake"
# Ninja ships in the same bin/ as cmake inside the cmake bundle.
NINJA="$(dirname "$CMAKE")/ninja"
[[ -x "$NINJA" ]] || NINJA="$(find "$TOOLS" -name ninja -perm /111 -maxdepth 5 2>/dev/null | head -1)"

# Wrapper: always inject the ninja path so cmake doesn't have to find it on PATH.
cmake() { "$CMAKE" -DCMAKE_MAKE_PROGRAM="$NINJA" "$@"; }

SYSROOT="$U/sysroot"
RT_LIB="$U/out/rt/lib/wasm32-unknown-unknown/libclang_rt.builtins.a"
BINARYEN="$TOOLS/binaryen/bin"
WASM_LD=/usr/bin/wasm-ld-19

WASM_TARGET=wasm32-unknown-unknown
WASM_TRIPLE_LINUX=wasm32-unknown-linux-musl

LLVM_TARGETS="WebAssembly;X86;AArch64;ARM;RISCV"
LLVM_PROJECTS="clang;lld"

# Tools to install into /usr/bin in the guest
LLVM_TOOLS="clang;opt;llc;llvm-mc;lld;llvm-readobj;llvm-objdump;llvm-nm;llvm-dwarfdump;llvm-symbolizer;llvm-objcopy"

FORCE=",${FORCE:-},"

# ── prerequisite checks ────────────────────────────────────────────────────────
check() {
  [[ -e "$1" ]] || { echo "missing: $1" >&2; echo "  $2" >&2; exit 1; }
}
check "$SYSROOT/lib/libc.a"    "run: ./build.sh userspace"
check "$RT_LIB"                "run: ./build.sh userspace"
check "$LLVM_SRC/llvm/CMakeLists.txt" "clone llvm-project to $ROOT/../org/llvm-project"
check "$CMAKE"                 ".tools/cmake missing; run: ./build.sh tools"
check "$NINJA"                 ".tools/cmake (ninja) missing; run: ./build.sh tools"
check "$BINARYEN/wasm-opt"     ".tools/binaryen missing; run: ./build.sh tools"
for t in clang-19 clang++-19 wasm-ld-19 llvm-ar-19 llvm-ranlib-19 llvm-nm-19 llvm-strip-19; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing: $t (apt install llvm-19 clang-19)" >&2; exit 1; }
done

# ── stage helpers ──────────────────────────────────────────────────────────────
stage() {
  local name=$1 out=$2; shift 2
  if [[ "$FORCE" == *",$name,"* || ! -e "$out" ]]; then
    echo "==== stage $name"; return 0
  fi
  for src in "$@"; do
    [[ "$src" -nt "$out" ]] && { echo "==== stage $name (source changed)"; return 0; }
  done
  echo "==== stage $name: up to date"; return 1
}

log_file() { echo "  (log: $1)"; }

mkdir -p "$B" "$OUT"

# ── stage native: llvm-tblgen + clang-tblgen for host ────────────────────────
NATIVE_BUILD="$B/native"
NATIVE_TBLGEN="$NATIVE_BUILD/bin/llvm-tblgen"
CLANG_TBLGEN="$NATIVE_BUILD/bin/clang-tblgen"

if stage native "$CLANG_TBLGEN" "$LLVM_SRC/llvm/utils/TableGen/TableGenBackends.h"; then
  LOG="$B/native.log"
  # Build only the tablegen tools; no LLVM tools needed.
  # Using native (host) compiler — do not cross-compile here.
  cmake -S "$LLVM_SRC/llvm" -B "$NATIVE_BUILD" -G Ninja \
    --log-level=WARNING \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
    -DLLVM_ENABLE_PROJECTS="$LLVM_PROJECTS" \
    -DLLVM_TARGETS_TO_BUILD=Native \
    -DLLVM_BUILD_TOOLS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_CURL=OFF \
    > "$LOG" 2>&1 || { tail -30 "$LOG"; log_file "$LOG"; exit 1; }
  "$NINJA" -C "$NATIVE_BUILD" -j"$(nproc)" llvm-tblgen clang-tblgen \
    >> "$LOG" 2>&1 || { tail -30 "$LOG"; log_file "$LOG"; exit 1; }
fi

# ── stage libcxx: libc++ / libc++abi for wasm32 ──────────────────────────────
# LLVM, clang and lld are built with -fno-exceptions, so the C++ runtime is built without
# exceptions too and needs no unwinder (libunwind's register save/restore assembly has no
# wasm port, and its Unwind-wasm.c clashes with -wasm-enable-sjlj over __cpp_exception).
LIBCXX_BUILD="$B/libcxx"
LIBCXX_PREFIX="$B/libcxx-install"
LIBCXX_A="$LIBCXX_PREFIX/lib/libc++.a"

# The flags wasm-cc uses: atomics + bulk memory (wasm-ld refuses --shared-memory without
# them in every object), setjmp/longjmp as wasm exceptions, and the Linux macros.
WASM_FLAGS="--target=$WASM_TARGET -matomics -mbulk-memory -mllvm -wasm-enable-sjlj -D__linux__=1 -D__linux=1 -D__unix__=1 -D__unix=1 -D__gnu_linux__=1 -Wno-unused-command-line-argument -Wno-override-module --sysroot=$SYSROOT"

WASM_LINK_FLAGS="--target=$WASM_TARGET -fuse-ld=$WASM_LD -nostdlib -Wl,--table-base=3 -Wl,--import-memory -Wl,--max-memory=4294967296 -Wl,--shared-memory -Wl,--export-table -Wl,-z,stack-size=8388608 -L$SYSROOT/lib"

# libc++ only supports the two latest clang releases, so trunk libc++ cannot be built by the
# host's clang-19. The runtimes come from the release matching the host compiler, exported
# from the same clone (the checkout itself is not touched).
RT_TAG=llvmorg-19.1.7
RT_SRC="$B/rt-src"
if stage rt-src "$RT_SRC/runtimes/CMakeLists.txt"; then
  rm -rf "$RT_SRC"; mkdir -p "$RT_SRC"
  git -C "$LLVM_SRC" archive "$RT_TAG" runtimes cmake llvm/cmake llvm/utils/llvm-lit \
      libcxx libcxxabi | tar -xm -C "$RT_SRC"
fi

# try_compile builds static libraries here, so link checks always pass: the options naming
# what musl lacks (__cxa_thread_atexit_impl) are given explicitly.
if stage libcxx "$LIBCXX_A" "$RT_SRC/runtimes/CMakeLists.txt"; then
  LOG="$B/libcxx.log"
  rm -rf "$LIBCXX_BUILD"
  cmake -S "$RT_SRC/runtimes" -B "$LIBCXX_BUILD" -G Ninja \
    --log-level=WARNING \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$LIBCXX_PREFIX" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
    -DCMAKE_AR=/usr/bin/llvm-ar-19 \
    -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-19 \
    -DCMAKE_C_COMPILER_TARGET="$WASM_TARGET" \
    -DCMAKE_CXX_COMPILER_TARGET="$WASM_TARGET" \
    "-DCMAKE_C_FLAGS=$WASM_FLAGS" \
    "-DCMAKE_CXX_FLAGS=$WASM_FLAGS" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXXABI_HAS_PTHREAD_API=ON \
    -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM=ON \
    -DLIBCXX_HAS_MUSL_LIBC=ON \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
    > "$LOG" 2>&1 || { tail -30 "$LOG"; log_file "$LOG"; exit 1; }
  "$NINJA" -C "$LIBCXX_BUILD" -j"$(nproc)" install \
    >> "$LOG" 2>&1 || { tail -30 "$LOG"; log_file "$LOG"; exit 1; }
fi

# ── stage src: the LLVM/Clang/LLD sources, with patches/ applied ─────────────
# Exported from the clone at a pinned commit (tests left out), so the clone itself is never
# modified. patches/*.patch port what this platform lacks (no fork, ...); see README.md.
LLVM_REV=4350d12c88cd947c3fb2b5e0bd4b22804e98a586
SRC="$B/src"
if stage src "$SRC/.stamp" "$A"/patches/*.patch; then
  rm -rf "$SRC"; mkdir -p "$SRC"
  # libc/: LLVM includes a few of llvm-libc's shared headers (FindLibcCommonUtils);
  # libunwind/include: lld's Mach-O port uses its compact-unwind header.
  git -C "$LLVM_SRC" archive "$LLVM_REV" llvm clang lld cmake third-party libc libunwind/include \
      ':(exclude)libc/test' ':(exclude)libc/docs' ':(exclude)libc/benchmarks' ':(exclude)libc/fuzzing' \
      ':(exclude)llvm/test' ':(exclude)clang/test' ':(exclude)lld/test' \
      ':(exclude)llvm/unittests' ':(exclude)clang/unittests' ':(exclude)lld/unittests' \
    | tar -xm -C "$SRC"
  for p in "$A"/patches/*.patch; do
    echo "  patch $(basename "$p")"
    patch -d "$SRC" -p1 --no-backup-if-mismatch -s < "$p"
  done
  touch "$SRC/.stamp"
fi

# ── stage cross: LLVM tools for wasm32 ────────────────────────────────────────
CROSS_BUILD="$B/cross"
# clang is the largest indicator that the cross build completed
CROSS_CLANG="$CROSS_BUILD/bin/clang"

LIBCXX_FLAGS="-stdlib=libc++ -I$LIBCXX_PREFIX/include/c++/v1 -I$LIBCXX_PREFIX/include"
LIBCXX_LINK="-L$LIBCXX_PREFIX/lib -lc++ -lc++abi"

if stage cross "$CROSS_CLANG" "$SRC/.stamp"; then
  LOG="$B/cross.log"
  # Configure once; later runs (after a patch) rebuild incrementally.
  [[ -f "$CROSS_BUILD/build.ninja" ]] || cmake -S "$SRC/llvm" -B "$CROSS_BUILD" -G Ninja \
    --log-level=WARNING \
    -DCMAKE_TOOLCHAIN_FILE="$A/cmake/wasm32-toolchain.cmake" \
    -DWASM_SYSROOT="$SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$CROSS_BUILD/install" \
    "-DCMAKE_CXX_FLAGS=$WASM_FLAGS -fno-exceptions $LIBCXX_FLAGS" \
    "-DCMAKE_C_FLAGS=$WASM_FLAGS" \
    "-DCMAKE_EXE_LINKER_FLAGS=$WASM_LINK_FLAGS $SYSROOT/lib/crt1.o" \
    "-DCMAKE_CXX_STANDARD_LIBRARIES=$LIBCXX_LINK -lc $RT_LIB" \
    "-DCMAKE_C_STANDARD_LIBRARIES=$LIBCXX_LINK -lc $RT_LIB" \
    -DLLVM_PARALLEL_LINK_JOBS=1 \
    -DLLVM_ENABLE_PROJECTS="$LLVM_PROJECTS" \
    -DLLVM_TARGETS_TO_BUILD="$LLVM_TARGETS" \
    -DLLVM_DEFAULT_TARGET_TRIPLE="$WASM_TRIPLE_LINUX" \
    -DLLVM_TABLEGEN="$NATIVE_TBLGEN" \
    -DCLANG_TABLEGEN="$CLANG_TBLGEN" \
    -DLLVM_NATIVE_TOOL_DIR="$NATIVE_BUILD/bin" \
    -DLLVM_BUILD_TOOLS=ON \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_CURL=OFF \
    -DLLVM_ENABLE_PIC=OFF \
    -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_BUILD_RUNTIME=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DCLANG_BUILD_TOOLS=ON \
    -DCLANG_TOOL_CLANG_FORMAT_BUILD=OFF \
    -DCLANG_TOOL_CLANG_CHECK_BUILD=OFF \
    -DCLANG_TOOL_CLANG_DIFF_BUILD=OFF \
    -DCLANG_TOOL_CLANG_EXTDEF_MAPPING_BUILD=OFF \
    -DCLANG_TOOL_CLANG_IMPORT_TEST_BUILD=OFF \
    -DCLANG_TOOL_CLANG_REFACTOR_BUILD=OFF \
    -DCLANG_TOOL_CLANG_REPL_BUILD=OFF \
    -DCLANG_TOOL_CLANG_SCAN_DEPS_BUILD=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DLLVM_HOST_TRIPLE="$WASM_TRIPLE_LINUX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLVM_TOOL_LTO_BUILD=OFF \
    -DLLVM_TOOL_LLVM_LTO_BUILD=OFF \
    -DLLVM_TOOL_LLVM_LTO2_BUILD=OFF \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=OFF \
    > "$B/cross-configure.log" 2>&1 || { tail -30 "$B/cross-configure.log"; log_file "$B/cross-configure.log"; exit 1; }
  "$NINJA" -C "$CROSS_BUILD" -j"${JOBS:-$(nproc)}" ${NINJA_FLAGS:-} ${LLVM_TOOLS//;/ } \
    > "$LOG" 2>&1 || { tail -30 "$LOG"; log_file "$LOG"; exit 1; }
fi

# ── stage cpio: strip → wasm-opt → pack ───────────────────────────────────────
CPIO_OUT="$OUT/llvm.cpio"

if stage cpio "$CPIO_OUT" "$CROSS_CLANG"; then
  STAGE="$B/stage"
  rm -rf "$STAGE"; mkdir -p "$STAGE/usr/bin"

  # Canonical tool names → wasm binaries
  declare -A BINS=(
    [clang]="$CROSS_BUILD/bin/clang"
    [opt]="$CROSS_BUILD/bin/opt"
    [llc]="$CROSS_BUILD/bin/llc"
    [llvm-mc]="$CROSS_BUILD/bin/llvm-mc"
    [lld]="$CROSS_BUILD/bin/lld"
    [llvm-readobj]="$CROSS_BUILD/bin/llvm-readobj"
    [llvm-objdump]="$CROSS_BUILD/bin/llvm-objdump"
    [llvm-nm]="$CROSS_BUILD/bin/llvm-nm"
    [llvm-dwarfdump]="$CROSS_BUILD/bin/llvm-dwarfdump"
    [llvm-symbolizer]="$CROSS_BUILD/bin/llvm-symbolizer"
    [llvm-objcopy]="$CROSS_BUILD/bin/llvm-objcopy"
  )

  for name in "${!BINS[@]}"; do
    src="${BINS[$name]}"
    # LLVM may write the binary without extension (wasm32 Linux outputs are just
    # ELF-named wasm; the kernel's binfmt recognises them by magic).
    [[ -f "$src" ]] || src="${src}.wasm"
    [[ -f "$src" ]] || { echo "missing tool binary: $name (looked for ${BINS[$name]})" >&2; exit 1; }
    dst="$STAGE/usr/bin/$name"
    echo "  strip + wasm-opt: $name"
    llvm-strip-19 -s "$src" -o "$dst.stripped"
    # Translate the withdrawn phase-3 SJLJ encoding (try/catch) to try_table/exnref.
    "$BINARYEN/wasm-opt" -all --translate-to-exnref "$dst.stripped" -o "$dst"
    rm "$dst.stripped"
    chmod 755 "$dst"
  done

  # Symlinks: clang++ → clang, ld.lld → lld, wasm-ld → lld,
  #           llvm-readelf → llvm-readobj, llvm-strip → llvm-objcopy
  ln -sf clang       "$STAGE/usr/bin/clang++"
  ln -sf lld         "$STAGE/usr/bin/ld.lld"
  ln -sf lld         "$STAGE/usr/bin/wasm-ld"
  ln -sf llvm-readobj "$STAGE/usr/bin/llvm-readelf"
  ln -sf llvm-objcopy "$STAGE/usr/bin/llvm-strip"

  echo "== packing cpio"
  MKARGS=(python3 "$U/mkinitramfs.py" --overlay -o "$CPIO_OUT")
  while IFS= read -r f; do
    rel="${f#$STAGE/}"
    if [[ -L "$f" ]]; then
      MKARGS+=(--link "${rel}=$(readlink "$f")")
    else
      MKARGS+=(--file "${rel}=${f}")
    fi
  done < <(find "$STAGE" \( -type f -o -type l \) | sort)
  "${MKARGS[@]}"

  mkdir -p "$OUT/licenses"
  # LLVM / Clang / LLD are Apache-2.0 with LLVM-exception.
  cp "$LLVM_SRC/llvm/LICENSE.TXT"  "$OUT/licenses/llvm-LICENSE.txt"
  cp "$LLVM_SRC/clang/LICENSE.TXT" "$OUT/licenses/clang-LICENSE.txt"
  cp "$LLVM_SRC/lld/LICENSE.TXT"   "$OUT/licenses/lld-LICENSE.txt"

  ls -lh "$CPIO_OUT"
fi
