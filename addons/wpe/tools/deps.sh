#!/usr/bin/env bash
# The C/C++ libraries WebKit needs, cross-built for the guest (wasm32 Linux, musl) as static
# libraries into build/sysroot (headers, lib/*.a, lib/pkgconfig), each from the source
# sources.lock pins. Also the host glib (its build tools: glib-mkenums, glib-compile-resources).
#
#   tools/deps.sh [STAGE...]     the named stages, else all (each skipped when up to date;
#                                FORCE=stage[,stage] redoes them)
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
B="$A/build"
P="$B/sysroot"                   # where the guest libraries go
W="$B/deps"                      # their builds
U="$ROOT/userspace"
PYDEPS="$ROOT/python/deps"
WK_SRC="${WK_SRC:-$(cd "$ROOT/.." && pwd)/org/wpewebkit-2.54.1}"   # the WPE WebKit release tarball, unpacked
JOBS="${JOBS:-$(nproc)}"
. "$A/tools/lib.sh"
export PATH="$B/host/bin:$PATH"

CC="$A/tools/wasm-wpe-cc"
CXX="$A/tools/wasm-wpe-c++"
AR=llvm-ar-19
RANLIB=llvm-ranlib-19
CFLAGS_BASE="-O2 -g0"
export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig:$P/share/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
mkdir -p "$P/lib/pkgconfig" "$P/include" "$W" "$B/cross"

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -f "$PYDEPS/lib/libssl.a" && -f "$PYDEPS/lib/libz.a" && -f "$PYDEPS/lib/libsqlite3.a" ]] \
  || { echo "missing OpenSSL/zlib/sqlite for the guest; run: ./build.sh python" >&2; exit 1; }

want() { # stage: is it among the ones asked for (none asked: all)?
  [[ ${#ASKED[@]} == 0 ]] && return 0
  local s; for s in "${ASKED[@]}"; do [[ "$s" == "$1" ]] && return 0; done; return 1
}
ASKED=("$@")

# ── cross files: meson and cmake see a Linux wasm32 machine ──────────────────────────────────────
cat > "$B/cross/meson.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$(command -v llvm-ar-19)'
ranlib = '$(command -v llvm-ranlib-19)'
strip = '$(command -v llvm-strip-19)'
nm = '$(command -v llvm-nm-19)'
pkg-config = '$(command -v pkg-config)'

[host_machine]
system = 'linux'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[properties]
needs_exe_wrapper = true
# what glib would run a test program to find out (musl's printf, va_copy, a downward stack)
have_c99_vsnprintf = true
have_c99_snprintf = true
have_unix98_printf = true
va_val_copy = true
growing_stack = false
have_strlcpy = true
have_proc_self_cmdline = true
pkg_config_libdir = ['$P/lib/pkgconfig', '$P/share/pkgconfig']

[built-in options]
default_library = 'static'
prefer_static = true
c_args = ['-O2', '-g0', '-I$P/include']
cpp_args = ['-O2', '-g0', '-I$P/include']
EOF
cat > "$B/cross/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
# the ABI probe cannot read these out of a wasm program
set(CMAKE_SIZEOF_VOID_P 4)
set(CMAKE_C_SIZEOF_DATA_PTR 4)
set(CMAKE_CXX_SIZEOF_DATA_PTR 4)
set(CMAKE_C_COMPILER "$CC")
set(CMAKE_CXX_COMPILER "$CXX")
set(CMAKE_AR "$(command -v llvm-ar-19)" CACHE FILEPATH "")
set(CMAKE_RANLIB "$(command -v llvm-ranlib-19)" CACHE FILEPATH "")
set(CMAKE_NM "$(command -v llvm-nm-19)" CACHE FILEPATH "")
set(CMAKE_STRIP "$(command -v llvm-strip-19)" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH "$P" "$U/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF CACHE BOOL "")
set(THREADS_PREFER_PTHREAD_FLAG OFF)
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_PTHREADS_INIT 1)
set(Threads_FOUND TRUE)
EOF
cat > "$B/cross/native.ini" <<EOF
[binaries]
c = 'clang-19'
cpp = 'clang++-19'
ar = '$(command -v llvm-ar-19)'
pkg-config = '$(command -v pkg-config)'

[properties]
pkg_config_libdir = ['/usr/lib/$(uname -m)-linux-gnu/pkgconfig', '/usr/lib/pkgconfig', '/usr/share/pkgconfig']
EOF
MESON_CROSS=(--cross-file "$B/cross/meson.ini" --native-file "$B/cross/native.ini" --prefix "$P" --libdir lib --buildtype release
             -Ddefault_library=static --wrap-mode nodownload)
CMAKE_CROSS=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$B/cross/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF
             "-DCMAKE_C_FLAGS=$CFLAGS_BASE" "-DCMAKE_CXX_FLAGS=$CFLAGS_BASE" -Wno-dev)
AUTOTOOLS_CROSS=(--host=wasm32-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" --prefix="$P"
                 --libdir="$P/lib" --enable-static --disable-shared)

meson_build() { # name srcdir [meson options...]
  local name="$1" src="$2"; shift 2
  local LOG="$W/$name.log"; : > "$LOG"
  rm -rf "$W/$name-build"
  log_run "$LOG" meson setup "$W/$name-build" "$src" "${MESON_CROSS[@]}" "$@"
  log_run "$LOG" ninja -C "$W/$name-build" -j"$JOBS"
  if [[ -n "${DESTDIR_INSTALL:-}" ]]; then
    # files for absolute guest paths (/etc/...): all of it into build/deps/<name>-dest, the
    # library part from there into the sysroot
    rm -rf "$W/$name-dest"
    log_run "$LOG" env DESTDIR="$W/$name-dest" ninja -C "$W/$name-build" install
    cp -a "$W/$name-dest$P/." "$P/"
  else
    log_run "$LOG" ninja -C "$W/$name-build" install
  fi
}
cmake_build() { # name srcdir [cmake options...]
  local name="$1" src="$2"; shift 2
  local LOG="$W/$name.log"; : > "$LOG"
  rm -rf "$W/$name-build"
  log_run "$LOG" cmake -S "$src" -B "$W/$name-build" "${CMAKE_CROSS[@]}" "$@"
  log_run "$LOG" ninja -C "$W/$name-build" -j"$JOBS"
  log_run "$LOG" ninja -C "$W/$name-build" install
}
autotools_build() { # name srcdir [configure options...]   (in the source folder)
  local name="$1" src="$2"; shift 2
  local LOG="$W/$name.log"; : > "$LOG"
  # unpacked with fresh times in no particular order: make the generated files newer than their
  # sources again, or make wants automake to remake them
  (cd "$src" && find . -type f -exec touch -d '-10 min' {} + && find . -name aclocal.m4 -exec touch -d '-5 min' {} + \
     && find . \( -name configure -o -name Makefile.in -o -name 'config.h.in' -o -name '*.info' \) -exec touch -d '-1 min' {} +)
  (cd "$src" && log_run "$LOG" env CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" CC_FOR_BUILD=clang-19 BUILD_CC=clang-19 \
       CFLAGS="$CFLAGS_BASE" CXXFLAGS="$CFLAGS_BASE" ./configure "${AUTOTOOLS_CROSS[@]}" "$@" \
   && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install)
}
src() { # name: unpack its source into build/deps/<name>-src with patches/<name>/ applied
  unpack "$1" "$W/$1-src"
  apply_patches "$W/$1-src" "$A"/patches/"$1"/*.patch
  echo "$W/$1-src"
}

# ── libcxx: libc++ / libc++abi for wasm32 (WebKit, ICU, harfbuzz, skia are C++) ─────────────────
# LLVM 19.1.7's runtimes (the release of the host's clang-19), static, without exceptions, as
# addons/llvm and addons/node build them.
LIBCXX="$B/libcxx-install"
RT_TARBALL=https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/llvm-project-19.1.7.src.tar.xz
RT_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
if want libcxx && stage libcxx "$LIBCXX/lib/libc++.a"; then
  RT_SRC="$B/rt-src"; RT_DIRS=(runtimes cmake llvm/cmake llvm/utils/llvm-lit libcxx libcxxabi)
  TB="$B/dl/$(basename "$RT_TARBALL")"
  # the node add-on has the same archive: no need to download it twice
  [[ -f "$TB" || ! -f "$A/../node/build/dl/$(basename "$RT_TARBALL")" ]] \
    || ln "$A/../node/build/dl/$(basename "$RT_TARBALL")" "$TB" 2>/dev/null \
    || cp "$A/../node/build/dl/$(basename "$RT_TARBALL")" "$TB"
  [[ -f "$TB" ]] || { curl -fL --retry 3 -o "$TB.part" "$RT_TARBALL" && mv "$TB.part" "$TB"; }
  echo "$RT_SHA256  $TB" | sha256sum -c --quiet
  rm -rf "$RT_SRC" "$B/libcxx" "$LIBCXX"; mkdir -p "$RT_SRC"
  tar -xJmf "$TB" -C "$RT_SRC" --strip-components=1 "${RT_DIRS[@]/#/llvm-project-19.1.7.src/}"
  WASM_FLAGS="--target=wasm32-unknown-unknown -matomics -mbulk-memory -mllvm -wasm-enable-sjlj -D__linux__=1 -D__linux=1 -D__unix__=1 -D__unix=1 -D__gnu_linux__=1 -Wno-unused-command-line-argument -Wno-override-module --sysroot=$U/sysroot -O2"
  LOG="$B/libcxx.log"; : > "$LOG"
  log_run "$LOG" cmake -S "$RT_SRC/runtimes" -B "$B/libcxx" -G Ninja \
    --log-level=WARNING -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LIBCXX" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=/usr/bin/clang-19 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
    -DCMAKE_AR=/usr/bin/llvm-ar-19 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-19 \
    -DCMAKE_C_COMPILER_TARGET=wasm32-unknown-unknown -DCMAKE_CXX_COMPILER_TARGET=wasm32-unknown-unknown \
    "-DCMAKE_C_FLAGS=$WASM_FLAGS" "-DCMAKE_CXX_FLAGS=$WASM_FLAGS" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx" -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXXABI_HAS_PTHREAD_API=ON -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM=ON -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
  log_run "$LOG" ninja -C "$B/libcxx" -j"$JOBS" install
fi

# ── compat: libwasmcompat.a (compat/wasm_compat.c) ─────────────────────────────────────────────
if want compat && { stage compat "$B/compat/libwasmcompat.a" || [[ "$A/compat/wasm_compat.c" -nt "$B/compat/libwasmcompat.a" ]]; }; then
  mkdir -p "$B/compat"
  "$U/bin/wasm-cc" -O2 -I"$A/compat/include" -c "$A/compat/wasm_compat.c" -o "$B/compat/wasm_compat.o"
  rm -f "$B/compat/libwasmcompat.a"
  $AR rcs "$B/compat/libwasmcompat.a" "$B/compat/wasm_compat.o"
fi

# ── base: zlib, OpenSSL, sqlite3 from the python step, with pkg-config files ───────────────────
if want base && stage base "$P/lib/pkgconfig/sqlite3.pc"; then
  cp -a "$PYDEPS/include/zlib.h" "$PYDEPS/include/zconf.h" "$PYDEPS/include/sqlite3.h" \
        "$PYDEPS/include/sqlite3ext.h" "$P/include/"
  rm -rf "$P/include/openssl"; cp -a "$PYDEPS/include/openssl" "$P/include/"
  cp -a "$PYDEPS/lib/libz.a" "$PYDEPS/lib/libssl.a" "$PYDEPS/lib/libcrypto.a" "$PYDEPS/lib/libsqlite3.a" "$P/lib/"
  zv="$(sed -n 's/#define ZLIB_VERSION "\(.*\)"/\1/p' "$P/include/zlib.h")"
  sv="$(sed -n 's/#define SQLITE_VERSION  *"\(.*\)"/\1/p' "$P/include/sqlite3.h")"
  ov="$(sed -n 's/^Version: //p' "$PYDEPS/lib/pkgconfig/openssl.pc")"
  pc() { printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: %s\nDescription: %s\nVersion: %s\nLibs: -L${libdir} %s\nCflags: -I${includedir}\n' "$P" "$1" "$1" "$2" "$3" > "$P/lib/pkgconfig/$4.pc"; }
  pc zlib "$zv" -lz zlib
  pc sqlite3 "$sv" -lsqlite3 sqlite3
  pc libcrypto "$ov" -lcrypto libcrypto
  printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: libssl\nDescription: libssl\nVersion: %s\nRequires.private: libcrypto\nLibs: -L${libdir} -lssl\nCflags: -I${includedir}\n' "$P" "$ov" > "$P/lib/pkgconfig/libssl.pc"
  printf 'prefix=%s\n\nName: OpenSSL\nDescription: OpenSSL\nVersion: %s\nRequires: libssl libcrypto\n' "$P" "$ov" > "$P/lib/pkgconfig/openssl.pc"
fi

# ── small libraries ────────────────────────────────────────────────────────────────────────────
if want pcre2 && stage pcre2 "$P/lib/libpcre2-8.a"; then
  S="$(src pcre2)"
  cmake_build pcre2 "$S" -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
    -DPCRE2_SUPPORT_JIT=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_UNICODE=ON
fi
if want expat && stage expat "$P/lib/libexpat.a"; then
  S="$(src expat)"
  cmake_build expat "$S" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF
fi
if want brotli && stage brotli "$P/lib/libbrotlidec.a"; then
  S="$(src brotli)"
  cmake_build brotli "$S" -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF
fi
if want libpng && stage libpng "$P/lib/libpng16.a"; then
  S="$(src libpng)"
  cmake_build libpng "$S" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF -DPNG_FRAMEWORK=OFF
fi
if want libjpeg-turbo && stage libjpeg-turbo "$P/lib/libjpeg.a"; then
  S="$(src libjpeg-turbo)"
  cmake_build libjpeg-turbo "$S" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF \
    -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
fi
if want libwebp && stage libwebp "$P/lib/libwebpdemux.a"; then
  S="$(src libwebp)"
  cmake_build libwebp "$S" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON -DWEBP_ENABLE_SIMD=OFF \
    -DWEBP_USE_THREAD=ON
fi
if want freetype && stage freetype "$P/lib/libfreetype.a"; then
  S="$(src freetype)"
  meson_build freetype "$S" -Dbrotli=enabled -Dbzip2=disabled -Dharfbuzz=disabled \
    -Dpng=enabled -Dzlib=system -Dtests=disabled
fi
if want fontconfig && stage fontconfig "$P/lib/libfontconfig.a"; then
  S="$(src fontconfig)"
  DESTDIR_INSTALL=1 meson_build fontconfig "$S" -Ddoc=disabled -Dtests=disabled -Dtools=disabled \
    -Dcache-build=disabled -Dnls=disabled -Dxml-backend=expat -Dsysconfdir=/etc -Dlocalstatedir=/var \
    -Ddefault-fonts-dirs=/usr/share/fonts -Dbaseconfig-dir=/etc/fonts -Dconfig-dir=/etc/fonts/conf.d \
    -Dtemplate-dir=/usr/share/fontconfig/conf.avail -Dcache-dir=/var/cache/fontconfig
fi
# ── ffi: libffi for glib's generic marshaller (ffi/: a call stub per wasm signature) ───────────
if want ffi && { stage ffi "$P/lib/libffi.a" || [[ "$A/ffi/ffi.c" -nt "$P/lib/libffi.a" || "$A/ffi/gen-calls.py" -nt "$P/lib/libffi.a" || "$A/ffi/ffi.h" -nt "$P/lib/libffi.a" || "$A/ffi/signatures.c" -nt "$P/lib/libffi.a" ]]; }; then
  mkdir -p "$W/ffi"
  python3 "$A/ffi/gen-calls.py" "$W/ffi/ffi_calls.c"
  "$CC" -O2 -I"$A/ffi" -c "$A/ffi/ffi.c" -o "$W/ffi/ffi.o"
  "$CC" -O2 -I"$A/ffi" -c "$A/ffi/signatures.c" -o "$W/ffi/signatures.o"
  "$CC" -O1 -I"$A/ffi" -c "$W/ffi/ffi_calls.c" -o "$W/ffi/ffi_calls.o"
  rm -f "$P/lib/libffi.a"; $AR rcs "$P/lib/libffi.a" "$W/ffi/ffi.o" "$W/ffi/signatures.o" "$W/ffi/ffi_calls.o"
  cp "$A/ffi/ffi.h" "$P/include/ffi.h"
  printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: libffi\nDescription: libffi for wasm32 (call stubs per signature)\nVersion: 3.4.8\nLibs: -L${libdir} -lffi\nCflags: -I${includedir}\n' "$P" > "$P/lib/pkgconfig/libffi.pc"
fi

# ── host-glib: glib for the build machine, for its tools (glib-mkenums, glib-compile-resources) ─
if want host-glib && stage host-glib "$B/host/bin/glib-compile-resources"; then
  S="$(src glib)"
  LOG="$W/host-glib.log"; : > "$LOG"; rm -rf "$W/glib-host"
  log_run "$LOG" env -u PKG_CONFIG_LIBDIR CC=clang-19 CXX=clang++-19 meson setup "$W/glib-host" "$S" \
    --prefix "$B/host" --libdir lib --buildtype release -Ddefault_library=static -Dtests=false \
    -Dintrospection=disabled -Dnls=disabled -Dman-pages=disabled -Ddocumentation=false \
    -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dsysprof=disabled \
    --wrap-mode=default --force-fallback-for=pcre2
  log_run "$LOG" ninja -C "$W/glib-host" -j"$JOBS"
  log_run "$LOG" ninja -C "$W/glib-host" install
fi

# ── icu: built for the build machine first (its tools make the data), then for the guest ────────
if want icu && stage icu "$P/lib/libicuuc.a"; then
  S="$(src icu)"
  LOG="$W/icu.log"; : > "$LOG"
  rm -rf "$W/icu-host" "$W/icu-build"; mkdir -p "$W/icu-host" "$W/icu-build"
  (cd "$W/icu-host" && log_run "$LOG" env CC=clang-19 CXX=clang++-19 CFLAGS=-O2 CXXFLAGS=-O2 \
       "$S/source/configure" --disable-shared --enable-static --disable-tests --disable-samples \
     && log_run "$LOG" make -j"$JOBS")
  (cd "$W/icu-build" && log_run "$LOG" env CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" \
       CFLAGS="$CFLAGS_BASE" CXXFLAGS="$CFLAGS_BASE -std=c++17" \
       "$S/source/configure" "${AUTOTOOLS_CROSS[@]}" --with-cross-build="$W/icu-host" \
       --disable-tools --disable-tests --disable-samples --disable-extras --disable-icuio \
       --with-data-packaging=static \
     && sed -i 's/^GENCCODE_ASSEMBLY *=.*/GENCCODE_ASSEMBLY =/' icudefs.mk \
     && log_run "$LOG" make -j"$JOBS" PKGDATA_OPTS="-O $W/icu-build/data/icupkg.inc -w" \
     && log_run "$LOG" make install PKGDATA_OPTS="-O $W/icu-build/data/icupkg.inc -w")
  # (the data as C, not as the assembler file or ELF object ICU makes on Linux: wasm has neither)
fi

# ── glib: GLib, GObject, GIO for the guest ───────────────────────────────────────────────────────
if want glib && stage glib "$P/lib/libgio-2.0.a"; then
  S="$(src glib)"
  meson_build glib "$S" -Dtests=false -Dintrospection=disabled -Dnls=disabled -Dman-pages=disabled \
    -Ddocumentation=false -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dsysprof=disabled \
    -Dglib_debug=disabled -Dlibelf=disabled
fi

# ── text, network and crypto libraries ─────────────────────────────────────────────────────────
if want harfbuzz && stage harfbuzz "$P/lib/libharfbuzz-icu.a"; then
  S="$(src harfbuzz)"
  meson_build harfbuzz "$S" -Dicu=enabled -Dfreetype=enabled -Dglib=enabled -Dgobject=disabled \
    -Dcairo=disabled -Dchafa=disabled -Dtests=disabled -Dintrospection=disabled -Ddocs=disabled \
    -Dutilities=disabled -Dbenchmark=disabled
fi
if want libxml2 && stage libxml2 "$P/lib/libxml2.a"; then
  S="$(src libxml2)"
  meson_build libxml2 "$S" -Dpython=disabled -Dicu=disabled -Dzlib=enabled -Dlegacy=disabled \
    -Dmodules=disabled -Dreadline=disabled -Dhistory=disabled -Diconv=enabled -Ddocs=disabled
fi
if want nghttp2 && stage nghttp2 "$P/lib/libnghttp2.a"; then
  S="$(src nghttp2)"
  cmake_build nghttp2 "$S" -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF -DBUILD_TESTING=OFF
fi
if want libpsl && stage libpsl "$P/lib/libpsl.a"; then
  S="$(src libpsl)"
  meson_build libpsl "$S" -Druntime=no -Dbuiltin=true -Ddocs=false -Dtests=false
fi
if want libsoup && stage libsoup "$P/lib/libsoup-3.0.a"; then
  S="$(src libsoup)"
  meson_build libsoup "$S" -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled -Dzstd=disabled \
    -Dtls_check=false -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled -Ddoc_tests=false \
    -Dtests=false -Dautobahn=disabled -Dinstalled_tests=false -Dsysprof=disabled -Dfuzzing=disabled \
    -Dpkcs11_tests=disabled
fi
if want glib-networking && stage glib-networking "$P/lib/gio/modules/libgioopenssl.a"; then
  S="$(src glib-networking)"
  meson_build glib-networking "$S" -Dgnutls=disabled -Dopenssl=enabled -Denvironment_proxy=disabled \
    -Dlibproxy=disabled -Dgnome_proxy=disabled -Dinstalled_tests=false
fi
if want libgpg-error && stage libgpg-error "$P/lib/libgpg-error.a"; then
  S="$(src libgpg-error)"
  autotools_build libgpg-error "$S" --disable-doc --disable-tests --disable-languages --disable-nls \
    --enable-install-gpg-error-config
fi
if want libgcrypt && stage libgcrypt "$P/lib/libgcrypt.a"; then
  S="$(src libgcrypt)"
  autotools_build libgcrypt "$S" --disable-asm --disable-doc --disable-jent-support \
    --with-libgpg-error-prefix="$P" --disable-padlock-support --disable-aesni-support \
    --disable-sse41-support --disable-avx-support --disable-avx2-support --disable-neon-support
fi
if want libtasn1 && stage libtasn1 "$P/lib/libtasn1.a"; then
  S="$(src libtasn1)"
  autotools_build libtasn1 "$S" --disable-doc --disable-valgrind-tests
fi
# the Khronos EGL/GLES headers (libepoxy and WebKit include them; nothing calls GL here): ANGLE's,
# from the WebKit source
if want khronos && stage khronos "$P/include/EGL/eglplatform.h"; then
  for d in EGL KHR GLES2 GLES3; do rm -rf "$P/include/$d"; cp -a "$WK_SRC/Source/ThirdParty/ANGLE/include/$d" "$P/include/"; done
fi
if want libepoxy && stage libepoxy "$P/lib/libepoxy.a"; then
  S="$(src libepoxy)"
  meson_build libepoxy "$S" -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false
fi
if want libxkbcommon && stage libxkbcommon "$P/lib/libxkbcommon.a"; then
  S="$(src libxkbcommon)"
  meson_build libxkbcommon "$S" -Denable-x11=false -Denable-tools=false -Denable-docs=false -Denable-wayland=false \
    -Denable-xkbregistry=false -Denable-bash-completion=false -Dxkb-config-root=/usr/share/X11/xkb \
    -Dx-locale-root=/usr/share/X11/locale
fi

echo "deps: done ($(ls "$P/lib"/*.a | wc -l) libraries in $P/lib)"
