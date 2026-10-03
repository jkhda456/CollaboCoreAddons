#!/usr/bin/env bash
# The jdk add-on: OpenJDK 25 for the guest — HotSpot's Zero interpreter and the JDK's native
# libraries for wasm32, linked statically into one `java` (and the other launchers), with the
# class library of a set of modules in lib/modules — packed as out/jdk.cpio (an overlay:
# /usr/lib/jvm/java-25-openjdk, /usr/bin/java, javac, jar, jshell, ...). README.md has the details.
#
# Needs the userspace step (musl, compiler-rt: userspace/sysroot). The source is the clone
# ../org/jdk25u at a pinned tag, exported to build/src/jdk with patches/jdk/*.patch applied. The
# build machine needs a JDK 25 to build with (the boot JDK) and autoconf; both are downloaded into
# build/ (tools stage).
#
#   addons/jdk/build.sh
#   FORCE=stage[,stage]    redo those stages: tools src libcxx deps configure libffi launcher
#                          jspawnhelper image cpio (java, native and launcher run the JDK's make
#                          every time; it knows what changed)
#   JOBS=N
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
ORG="$(cd "$ROOT/.." && pwd)/org"
B="$A/build"
S="$B/src"
JOBS="${JOBS:-$(nproc)}"
FORCE=",${FORCE:-},"

JDK_SRC="$ORG/jdk25u"
JDK_TAG=jdk-25.0.5+7
# the boot JDK (and the build JDK of the cross build): Temurin 25 for the build machine
case "$(uname -m)" in
  aarch64) BOOT_ARCH=aarch64; BOOT_SHA256=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
  x86_64)  BOOT_ARCH=x64;     BOOT_SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
  *) echo "unsupported build machine $(uname -m)" >&2; exit 1 ;;
esac
BOOT_VER=25.0.4.1_1
BOOT_URL="https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_${BOOT_ARCH}_linux_hotspot_${BOOT_VER}.tar.gz"
AUTOCONF_URL=https://ftp.gnu.org/gnu/autoconf/autoconf-2.72.tar.xz
AUTOCONF_SHA256=ba885c1319578d6c94d46e9b0dceb4014caafe2490e437a0dbca3f270a223f5a
RT_TAG=llvmorg-19.1.7       # libc++ for the guest: the release of the host's clang-19
RT_TARBALL=https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/llvm-project-19.1.7.src.tar.xz
RT_TARBALL_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501

stage() { # name output...: true when the stage must run
  local name="$1"; shift
  if [[ "$FORCE" != *",$name,"* ]]; then
    local f ok=1
    for f in "$@"; do [[ -e "$f" ]] || ok=0; done
    if [[ $ok == 1 ]]; then echo "== $name: up to date"; return 1; fi
  fi
  echo "== $name"; return 0
}
redo() { FORCE="$FORCE$1,"; } # name: the stage must run (its inputs changed)
log_run() { # log command...
  local log="$1"; shift
  "$@" >> "$log" 2>&1 || { tail -40 "$log"; echo "failed: $* (log: $log)" >&2; exit 1; }
}
fetch() { # url sha256 file: download into build/dl (once) and check it
  local url="$1" sum="$2" f="$B/dl/$3"
  mkdir -p "$B/dl"
  if [[ ! -f "$f" ]]; then
    curl -fL --retry 3 -o "$f.part" "$url"
    mv "$f.part" "$f"
  fi
  echo "$sum  $f" | sha256sum -c --quiet || { echo "checksum mismatch: $f" >&2; exit 1; }
}

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -d "$JDK_SRC/.git" ]] || { echo "missing the clone $JDK_SRC" >&2; exit 1; }
mkdir -p "$B" "$A/out"

# ── tools: autoconf and the boot JDK, for the build machine ─────────────────────────────────────
T="$B/tools"
BOOT_JDK="$T/bootjdk"
if stage tools "$T/autoconf/bin/autoconf" "$BOOT_JDK/bin/javac"; then
  rm -rf "$T"; mkdir -p "$T"
  fetch "$AUTOCONF_URL" "$AUTOCONF_SHA256" autoconf-2.72.tar.xz
  tar -xJf "$B/dl/autoconf-2.72.tar.xz" -C "$T"
  LOG="$T/autoconf.log"; : > "$LOG"
  (cd "$T/autoconf-2.72" && log_run "$LOG" ./configure --prefix="$T/autoconf" \
   && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install)
  rm -rf "$T/autoconf-2.72"
  fetch "$BOOT_URL" "$BOOT_SHA256" "bootjdk-$BOOT_ARCH-$BOOT_VER.tar.gz"
  mkdir -p "$BOOT_JDK"
  tar -xzf "$B/dl/bootjdk-$BOOT_ARCH-$BOOT_VER.tar.gz" -C "$BOOT_JDK" --strip-components=1
fi
export PATH="$T/autoconf/bin:$PATH"

# ── src: the JDK at $JDK_TAG with patches/jdk applied ───────────────────────────────────────────
# A git repository whose first commit is the pristine export, so `git diff` there shows (and
# regenerates) the add-on's patch.
JS="$S/jdk"
if stage src "$JS/.stamp"; then
  rm -rf "$JS"; mkdir -p "$JS"
  git -C "$JDK_SRC" archive "$JDK_TAG" ADDITIONAL_LICENSE_INFO ASSEMBLY_EXCEPTION LICENSE Makefile \
      README.md configure make src bin doc | tar -xm -C "$JS"
  git -C "$JS" init -q && git -C "$JS" add -A
  git -C "$JS" -c user.name=build -c user.email=build@localhost commit -q -m pristine
  for p in "$A"/patches/jdk/*.patch; do
    [[ -f "$p" ]] || continue
    echo "  patch $(basename "$p")"
    git -C "$JS" apply --whitespace=nowarn "$p"
  done
  touch "$JS/.stamp"
fi

# ── libcxx: libc++ / libc++abi for wasm32 (HotSpot is C++) ──────────────────────────────────────
# As addons/llvm and addons/node build it: LLVM 19.1.7's runtimes, no exceptions, no unwinder.
LIBCXX="$B/libcxx-install"
if stage libcxx "$LIBCXX/lib/libc++.a" "$B/rt-src/libcxx/LICENSE.TXT"; then
  CMAKE="$ROOT/.tools/cmake/bin/cmake"
  NINJA="$(dirname "$CMAKE")/ninja"
  [[ -x "$NINJA" ]] || NINJA="$(find "$ROOT/.tools" -maxdepth 5 -name ninja -perm /111 2>/dev/null | head -1)"
  [[ -x "$CMAKE" && -n "$NINJA" ]] || { echo ".tools/cmake missing; run: ./build.sh tools" >&2; exit 1; }
  RT_SRC="$B/rt-src"
  RT_DIRS=(runtimes cmake llvm/cmake llvm/utils/llvm-lit libcxx libcxxabi)
  rm -rf "$RT_SRC" "$B/libcxx" "$LIBCXX"; mkdir -p "$RT_SRC"
  # the node add-on downloads the same archive: take its copy when there is one
  NODE_TB="$A/../node/build/dl/$(basename "$RT_TARBALL")"
  [[ -f "$B/dl/$(basename "$RT_TARBALL")" || ! -f "$NODE_TB" ]] || { mkdir -p "$B/dl"; cp "$NODE_TB" "$B/dl/"; }
  fetch "$RT_TARBALL" "$RT_TARBALL_SHA256" "$(basename "$RT_TARBALL")"
  tar -xJmf "$B/dl/$(basename "$RT_TARBALL")" -C "$RT_SRC" --strip-components=1 \
    "${RT_DIRS[@]/#/llvm-project-19.1.7.src/}"
  WASM_FLAGS="--target=wasm32-unknown-unknown -matomics -mbulk-memory -mllvm -wasm-enable-sjlj -D__linux__=1 -D__linux=1 -D__unix__=1 -D__unix=1 -D__gnu_linux__=1 -Wno-unused-command-line-argument -Wno-override-module --sysroot=$U/sysroot"
  LOG="$B/libcxx.log"; : > "$LOG"
  log_run "$LOG" "$CMAKE" -DCMAKE_MAKE_PROGRAM="$NINJA" -S "$RT_SRC/runtimes" -B "$B/libcxx" -G Ninja \
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
  log_run "$LOG" "$NINJA" -C "$B/libcxx" -j"$JOBS" install
fi

# ── deps: what the JDK's configure asks for on Linux ────────────────────────────────────────────
# libffi: Zero's calls into native methods go through ffi_call; ffi/ is a libffi for wasm32 whose
# call stubs are generated (tools/gen-ffi-calls.py). This first one has the generic stubs only; the
# libffi stage after the class library adds the signatures of its native methods.
# cups, fontconfig, alsa: configure wants their headers for java.desktop, which is not built for the
# guest (no AWT, no sound), so empty headers stand in for them.
# compat: libwasmcompat.a (compat/), mmap and the like for the JDK's code; tools/wasm-jdk-cc links it.
DEPS="$B/deps"
CC_W="$A/tools/wasm-jdk-cc"
build_libffi() { # classes-dir...
  local d="$DEPS/libffi" o="$B/ffi"
  mkdir -p "$d/include" "$d/lib" "$o"
  cp "$A/ffi/ffi.h" "$d/include/"
  python3 "$A/tools/gen-ffi-calls.py" "$o/ffi_calls.c" "$@"
  "$CC_W" -O2 -I"$A/ffi" -c "$A/ffi/ffi.c" -o "$o/ffi.o"
  "$CC_W" -O2 -I"$A/ffi" -c "$o/ffi_calls.c" -o "$o/ffi_calls.o"
  rm -f "$d/lib/libffi.a"
  llvm-ar-19 rcs "$d/lib/libffi.a" "$o/ffi.o" "$o/ffi_calls.o"
}
build_compat() {
  mkdir -p "$DEPS/compat"
  "$U/bin/wasm-cc" -O2 -Wall -I"$A/compat/include" -c "$A/compat/wasm_compat.c" -o "$DEPS/compat/wasm_compat.o"
  rm -f "$DEPS/compat/libwasmcompat.a"
  llvm-ar-19 rcs "$DEPS/compat/libwasmcompat.a" "$DEPS/compat/wasm_compat.o"
}
[[ "$A/compat/wasm_compat.c" -nt "$DEPS/compat/libwasmcompat.a" ]] && redo deps
if stage deps "$DEPS/libffi/lib/libffi.a" "$DEPS/stub/include/cups/cups.h" "$DEPS/compat/libwasmcompat.a"; then
  rm -rf "$DEPS/stub" "$DEPS/compat"
  build_compat
  [[ -f "$DEPS/libffi/lib/libffi.a" ]] || build_libffi
  for h in cups/cups.h cups/ppd.h fontconfig/fontconfig.h alsa/asoundlib.h; do
    mkdir -p "$DEPS/stub/include/$(dirname "$h")"
    echo "/* not used: java.desktop's native libraries are not built for the guest */" > "$DEPS/stub/include/$h"
  done
  mkdir -p "$DEPS/stub/lib"
fi

# ── configure: the JDK's build for the guest (a cross build: target wasm32-unknown-linux-musl) ──
# Zero (the interpreter, no JIT: the guest cannot make code at run time), Serial GC, no CDS (it maps
# its archive), headless; the boot JDK also stands in for the build JDK. tools/wasm-jdk-cc is the
# compiler. Only the static libraries are built (`*-static-libs`): the guest loads no libraries.
JB="$B/jdk"
CONF=(--openjdk-target=wasm32-unknown-linux-musl
      --with-jvm-variants=zero --with-jvm-features=-cds,-g1gc,-parallelgc,-vm-structs
      --disable-fallback-linker --enable-headless-only
      --with-boot-jdk="$BOOT_JDK" --with-build-jdk="$BOOT_JDK"
      --with-toolchain-type=clang
      CC="$A/tools/wasm-jdk-cc" CXX="$A/tools/wasm-jdk-c++" AR=llvm-ar-19 NM=llvm-nm-19
      OBJCOPY=llvm-objcopy-19 STRIP=llvm-strip-19
      BUILD_CC=clang-19 BUILD_CXX=clang++-19 BUILD_AR=llvm-ar-19 BUILD_NM=llvm-nm-19
      BUILD_OBJCOPY=llvm-objcopy-19 BUILD_STRIP=llvm-strip-19
      --with-libffi="$DEPS/libffi" --with-cups="$DEPS/stub" --with-fontconfig="$DEPS/stub"
      --with-alsa="$DEPS/stub" --with-freetype=bundled --with-zlib=bundled
      --with-native-debug-symbols=none --disable-warnings-as-errors --with-stdc++lib=dynamic
      --with-debug-level=release --with-hsdis=none --with-jtreg=no --with-gtest=no
      --with-vendor-name=collaboCore --with-vendor-version-string=collaboCore-WASM
      --with-version-pre= --with-version-opt= --with-version-build=7)
CONF_SIG="$(printf '%s\n' "${CONF[@]}" | sha256sum | cut -c1-16)"
[[ "$(cat "$JB/.conf-sig" 2>/dev/null)" != "$CONF_SIG" ]] && redo configure
# the JDK's make refuses a configuration older than its source (a new export, say)
[[ "$JS/.stamp" -nt "$JB/spec.gmk" ]] && redo configure
if stage configure "$JB/spec.gmk"; then
  mkdir -p "$JB"
  LOG="$B/configure.log"; : > "$LOG"
  (cd "$JB" && log_run "$LOG" bash "$JS/configure" "${CONF[@]}")
  echo "$CONF_SIG" > "$JB/.conf-sig"
  rm -f "$JB/.targets"
fi

# The modules of the image: the JDK without its desktop and GUI modules (java.desktop and what
# needs it), JFR, the Graal/JVMCI and serviceability-agent modules, jpackage, and the modules
# for native libraries the guest does not have (SCTP, smart cards, PKCS#11).
MODULES=(java.base java.compiler java.instrument java.logging java.management java.management.rmi
  java.naming java.net.http java.prefs java.rmi java.scripting java.security.jgss java.security.sasl
  java.sql java.sql.rowset java.transaction.xa java.xml java.xml.crypto
  jdk.attach jdk.charsets jdk.compiler jdk.crypto.ec jdk.dynalink jdk.httpserver jdk.internal.ed
  jdk.internal.jvmstat jdk.internal.le jdk.internal.md jdk.internal.opt jdk.jartool jdk.javadoc
  jdk.jcmd jdk.jdeps jdk.jdi jdk.jdwp.agent jdk.jlink jdk.jshell jdk.localedata jdk.management
  jdk.management.agent jdk.naming.dns jdk.naming.rmi jdk.net jdk.nio.mapmode jdk.security.auth
  jdk.security.jgss jdk.unsupported jdk.xml.dom jdk.zipfs)

jmake() { # log target...: the JDK's make (it knows itself what is up to date)
  local log="$1"; shift
  : > "$log"
  log_run "$log" make -C "$JB" JOBS="$JOBS" LOG=warn "$@"
}
# the make targets that exist, of these kinds, for MODULES
[[ -s "$JB/.targets" ]] || make -C "$JB" print-targets 2>/dev/null | tr ' ' '\n' > "$JB/.targets"
module_targets() { # kind...
  local m k
  for m in "${MODULES[@]}"; do for k in "$@"; do
    grep -qx "$m-$k" "$JB/.targets" && echo "$m-$k"
  done; done
}

# ── java: the class library, and the modules' generated sources, configuration and data ─────────
echo "== java"
jmake "$B/make-java.log" $(module_targets java copy gendata) release-file

# ── libffi: with the call stubs for the native methods of the image's classes ──────────────────
LIBFFI="$DEPS/libffi/lib/libffi.a"
CLASS_DIRS=("${MODULES[@]/#/$JB/jdk/modules/}")
[[ -f "$B/ffi/.classes" && -n "$(find "${CLASS_DIRS[@]}" -name '*.class' -newer "$B/ffi/.classes" -print -quit)" ]] && redo libffi
[[ "$A/ffi/ffi.c" -nt "$LIBFFI" || "$A/tools/gen-ffi-calls.py" -nt "$LIBFFI" ]] && redo libffi
if stage libffi "$B/ffi/.classes"; then
  build_libffi "${CLASS_DIRS[@]}"
  touch "$B/ffi/.classes"
fi

# ── native: HotSpot and the modules' native libraries, as static libraries ──────────────────────
echo "== native"
jmake "$B/make-native.log" hotspot-zero-static-libs $(module_targets static-libs)

# ── launcher: bin/java, linked with all of it; lib/jspawnhelper (Process: posix_spawn runs it) ────
# The JDK's make does not see libffi.a, libwasmcompat.a or a static library changing: relink then.
JAVA_EXE="$JB/support/static-native/launcher/java"
for f in "$LIBFFI" "$DEPS/compat/libwasmcompat.a" "$A/tools/wasm-jdk-cc" "$A/tools/gen-symtab.py" \
         "$JB"/hotspot/variant-zero/libjvm/objs/static/libjvm.a "$JB"/support/native/*/*/static/*.a; do
  [[ -f "$JAVA_EXE" && "$f" -nt "$JAVA_EXE" ]] && rm -f "$JAVA_EXE"
done
[[ "$FORCE" == *",launcher,"* ]] && rm -f "$JAVA_EXE"
echo "== launcher"
jmake "$B/make-launcher.log" static-launcher-only
SPAWN="$B/launcher/jspawnhelper"
CHILDPROC="$JB/support/native/java.base/libjava/static/childproc.o"
[[ "$CHILDPROC" -nt "$SPAWN" ]] && redo jspawnhelper
if stage jspawnhelper "$SPAWN"; then
  mkdir -p "$B/launcher"
  VERSION_STRING="$(sed -n 's/^VERSION_STRING := //p' "$JB/spec.gmk")"
  "$CC_W" -O2 -D_GNU_SOURCE -DLINUX -D_FILE_OFFSET_BITS=64 "-DVERSION_STRING=\"$VERSION_STRING\"" \
    -I"$JS/src/java.base/unix/native/libjava" -I"$JS/src/java.base/share/native/libjava" \
    -I"$JS/src/java.base/unix/native/include" -I"$JS/src/java.base/share/native/include" \
    -o "$SPAWN" "$JS/src/java.base/unix/native/jspawnhelper/jspawnhelper.c" "$CHILDPROC"
fi

# ── image: the runtime image, as jlink makes it from the modules (jmods made here) ──────────────
# lib/modules holds the classes; bin/java is the static launcher, and the JDK's tools in bin/ are
# links to it (its main.c picks the tool by the name it is run as). The jmods name the platform
# linux-other: jlink knows no wasm32, and at run time the JDK calls this architecture OTHER too.
IMG="$B/image"
JAVA_HOME_GUEST=usr/lib/jvm/java-25-openjdk
TOOLS=(jar jarsigner javac javadoc javap jcmd jdb jdeprscan jdeps jimage jinfo jlink jmap jmod
       jnativescan jps jshell jstack jstat jwebserver keytool kinit klist ktab rmiregistry serialver)
if [[ -f "$IMG/lib/modules" ]] && [[ "$JAVA_EXE" -nt "$IMG/bin/java" || "$SPAWN" -nt "$IMG/lib/jspawnhelper" \
   || -n "$(find "${CLASS_DIRS[@]}" "$JB/support/modules_libs" "$JB/support/modules_conf" -newer "$IMG/lib/modules" -print -quit)" ]]; then
  redo image
fi
if stage image "$IMG/bin/java" "$IMG/lib/modules" "$IMG/lib/jspawnhelper"; then
  JMODS="$B/jmods"
  rm -rf "$JMODS" "$IMG"; mkdir -p "$JMODS"
  VERSION_NUMBER="$(sed -n 's/^VERSION_NUMBER := //p' "$JB/spec.gmk")"
  for m in "${MODULES[@]}"; do
    a=(--module-version "$VERSION_NUMBER" --target-platform linux-other --class-path "$JB/jdk/modules/$m")
    [[ -d "$JB/support/modules_libs/$m" ]] && a+=(--libs "$JB/support/modules_libs/$m")
    [[ -d "$JB/support/modules_conf/$m" ]] && a+=(--config "$JB/support/modules_conf/$m")
    legal="$JB/support/modules_legal/common"
    [[ -d "$JB/support/modules_legal/$m" ]] && legal="$legal:$JB/support/modules_legal/$m"
    "$BOOT_JDK/bin/jmod" create "${a[@]}" --legal-notices "$legal" "$JMODS/$m.jmod"
  done
  # the release file: the JDK's, naming the source as the clone's tag and this add-on's patches
  sed "s|^SOURCE=.*|SOURCE=\"jdk25u:$JDK_TAG + addons/jdk/patches\"|" "$JB/jdk/release" > "$B/release"
  LOG="$B/jlink.log"; : > "$LOG"
  # lib/modules compressed (zip-6): half the size; reading classes is no slower for it here
  log_run "$LOG" "$BOOT_JDK/bin/jlink" --module-path "$JMODS" --add-modules "$(IFS=,; echo "${MODULES[*]}")" \
    --output "$IMG" --no-header-files --no-man-pages --compress zip-6 --release-info "$B/release"
  mkdir -p "$IMG/bin"
  cp "$JAVA_EXE" "$IMG/bin/java"
  chmod 755 "$IMG/bin/java"
  for t in "${TOOLS[@]}"; do ln -s java "$IMG/bin/$t"; done
  cp "$SPAWN" "$IMG/lib/jspawnhelper"
  chmod 755 "$IMG/lib/jspawnhelper"
fi

# ── cpio: the overlay ────────────────────────────────────────────────────────────────────────────
# /usr/lib/jvm/java-25-openjdk (JAVA_HOME), /usr/bin/java and the tools as links into it.
CPIO="$A/out/jdk.cpio"
[[ -f "$CPIO" && -n "$(find "$IMG" -newer "$CPIO" -print -quit)" ]] && redo cpio
if stage cpio "$CPIO"; then
  rm -rf "$A/out/licenses"; mkdir -p "$A/out/licenses"
  cp "$JS/LICENSE" "$A/out/licenses/addon-jdk-openjdk.txt"
  cp "$JS/ASSEMBLY_EXCEPTION" "$A/out/licenses/addon-jdk-openjdk-assembly-exception.txt"
  cp "$JS/ADDITIONAL_LICENSE_INFO" "$A/out/licenses/addon-jdk-openjdk-additional-license-info.txt"
  cp "$B/rt-src/libcxx/LICENSE.TXT" "$A/out/licenses/addon-jdk-libcxx.txt"
  links=()
  for t in java "${TOOLS[@]}"; do links+=(--link "usr/bin/$t=../lib/jvm/java-25-openjdk/bin/$t"); done
  python3 "$U/mkinitramfs.py" --overlay -o "$CPIO" --tree "$JAVA_HOME_GUEST=$IMG" "${links[@]}"
fi
ls -l "$CPIO" "$IMG/bin/java"
