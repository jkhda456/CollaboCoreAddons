#!/usr/bin/env bash
# WPE WebKit for the guest: the release tarball's source (WK_SRC, ../org/wpewebkit-2.54.1 by
# default; never modified) exported into build/src/webkit with patches/webkit/*.patch applied,
# configured for wasm32 (CLoop JavaScriptCore, Skia on the CPU, no GL, no media), built as static
# libraries in build/webkit.
#
#   tools/webkit.sh [src|configure|build]...   (default: all three; FORCE=src re-exports the source)
#   JOBS=N                                     compile jobs (default 4: a unified C++ source of
#                                              WebCore takes 1-2 GB of memory)
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
B="$A/build"
P="$B/sysroot"
S="$B/src/webkit"
O="$B/webkit"
WK_SRC="${WK_SRC:-$(cd "$ROOT/.." && pwd)/org/wpewebkit-2.54.1}"
JOBS="${JOBS:-4}"
. "$A/tools/lib.sh"
export PATH="$B/host/bin:$PATH"
# what WebKit's generator scripts run to preprocess (the inspector protocol's preprocess.pl looks at
# CC, then /usr/bin/clang or gcc, which this machine may not have)
export CC=clang-19 CXX=clang++-19
export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig:$P/share/pkgconfig:$P/lib/gio/modules/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
steps=("$@"); [[ ${#steps[@]} -gt 0 ]] || steps=(src configure build)

for step in "${steps[@]}"; do
  case "$step" in
  src)
    # A git repository whose first commit is the pristine release: `git diff` there is our change
    # (tools/save-patches.sh writes it back to patches/webkit).
    if stage webkit-src "$S/.git"; then
      [[ -f "$WK_SRC/Source/cmake/OptionsWPE.cmake" ]] || { echo "no WPE WebKit source at $WK_SRC" >&2; exit 1; }
      rm -rf "$S"; mkdir -p "$S"
      (cd "$WK_SRC" && tar -cf - --exclude=./Source/ThirdParty/ANGLE/third_party \
          --exclude=./Source/ThirdParty/gtest --exclude=./Tools/MiniBrowser --exclude=./Documentation .) \
        | tar -xmf - -C "$S"
      git -C "$S" init -q
      git -C "$S" add -A
      git -C "$S" -c user.name=build -c user.email=build@localhost commit -q -m "WPE WebKit $(basename "$WK_SRC") (pristine)"
      for p in "$A"/patches/webkit/*.patch; do
        [[ -f "$p" ]] || continue
        echo "  patch $(basename "$p")"
        git -C "$S" apply --whitespace=nowarn "$p" || { echo "patch failed: $p" >&2; exit 1; }
      done
    fi ;;
  configure)
    LOG="$B/webkit-configure.log"; : > "$LOG"
    mkdir -p "$O"
    log_run "$LOG" cmake -S "$S" -B "$O" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$B/cross/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$P" \
      "-DCMAKE_C_FLAGS=-O2 -g0 -msimd128 -I$P/include" "-DCMAKE_CXX_FLAGS=-O2 -g0 -msimd128 -I$P/include" \
      -DPORT=WPE -DDEVELOPER_MODE=OFF -DUSE_GSTREAMER=OFF -DCMAKE_SKIP_RPATH=ON -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
      -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
      -DENABLE_WPE_PLATFORM_COLLABO=ON -DCOLLABO_GUI_CLIENT_DIR="$ROOT/tools/gui/client" \
      -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
      -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
      -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_MINIBROWSER=OFF -DENABLE_API_TESTS=OFF \
      -DENABLE_LAYOUT_TESTS=OFF -DENABLE_WEBDRIVER=OFF -DENABLE_COG=OFF \
      -DUSE_ATK=OFF -DUSE_FLITE=OFF -DENABLE_SPEECH_SYNTHESIS=OFF -DUSE_GBM=OFF -DUSE_LIBDRM=OFF \
      -DUSE_LIBBACKTRACE=OFF -DUSE_LIBHYPHEN=OFF -DUSE_VULKAN=OFF -DUSE_AVIF=OFF -DUSE_LCMS=OFF \
      -DUSE_JPEGXL=OFF -DUSE_WOFF2=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_SYSTEM_UNIFDEF=ON -DUNIFDEF_EXECUTABLE="$B/host/bin/unifdef" \
      -DENABLE_SPELLCHECK=OFF -DENABLE_XSLT=OFF -DENABLE_PDFJS=OFF \
      -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF \
      -DENABLE_WEB_CODECS=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SESSION=OFF \
      -DENABLE_GAMEPAD=OFF -DENABLE_WEBXR=OFF -DENABLE_WEBGL=OFF -DENABLE_GPU_PROCESS=OFF \
      -DENABLE_THUNDER=OFF -DENABLE_JIT=OFF -DENABLE_C_LOOP=ON -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF \
      -DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF -DUSE_SYSTEM_MALLOC=ON \
      -DENABLE_RESOURCE_USAGE=OFF -DENABLE_MEMORY_SAMPLER=OFF -DENABLE_REMOTE_INSPECTOR=OFF
    ;;
  build)
    LOG="$B/webkit-build.log"
    echo "== webkit: ninja -j$JOBS (log: $LOG)"
    # the library and every object the add-on's program links (tools/browser.sh; lib.sh
    # webkit_objects), without linking WebKit's own programs
    mapfile -t objects < <(webkit_objects "$O"; webkit_bmalloc_objects "$O")
    ninja -C "$O" -j"$JOBS" ${NINJA_FLAGS:-} WebKit lib/libPAL.a lib/libSkia.a lib/libxdgmime.a "${objects[@]}" 2>&1 | tee "$LOG" | grep -E "^FAILED|error:|^ninja: build stopped" | head -50
    [[ ${PIPESTATUS[0]} == 0 ]] || { echo "webkit build failed (log: $LOG)" >&2; exit 1; }
    # the API headers, as an installation has them, for the add-on's programs (tools/browser.sh)
    rm -rf "$B/stage"
    DESTDIR="$B/stage" cmake --install "$O" --component Development > "$B/webkit-install.log"
    ;;
  *) echo "unknown step: $step" >&2; exit 2 ;;
  esac
done
