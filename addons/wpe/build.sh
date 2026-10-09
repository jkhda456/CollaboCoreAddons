#!/usr/bin/env bash
# The wpe add-on: WPE WebKit 2.54 for the guest — a web browser agents drive with the `web` command,
# shown on the guest's display (`gui`, tools/gui) — packed as out/wpe.cpio (an overlay):
#
#   /usr/bin/web                         the command agents use (browser/web.c)
#   /usr/bin/wpe-browser                 the browser: every WebKit process in one program
#   /usr/libexec/wpe-webkit-2.0/WPE{Web,Network}Process   links to it, where WebKit starts them
#   /etc/fonts, /usr/share/fonts         fontconfig and the fonts (DejaVu, Nanum Gothic for Korean)
#   /usr/share/mime/globs2               file types by name (share/mime: the web's common ones)
#
# Stages (each skipped when up to date; FORCE=stage[,stage] redoes one):
#   host     tools the build runs here: meson, ruby, gperf, unifdef, host glib    tools/host-tools.sh
#   deps     the C/C++ libraries WebKit needs, for wasm32 (build/sysroot)         tools/deps.sh
#   webkit   WebKit itself (build/webkit): a long build, hours on 6 cores          tools/webkit.sh
#   browser  wpe-browser.wasm and web.wasm (build/browser)                         tools/browser.sh
#   cpio     out/wpe.cpio and out/licenses
#
#   addons/wpe/build.sh [STAGE...]       JOBS=N   WK_SRC=DIR (../org/wpewebkit-2.54.1)
#
# Needs: ./build.sh userspace python (musl and compiler-rt; zlib, OpenSSL, sqlite for the guest),
# clang-19 / lld-19, cmake and ninja from .tools, python3, perl; network access for the downloads.
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
B="$A/build"
U="$ROOT/userspace"
. "$A/tools/lib.sh"
export JOBS="${JOBS:-$(nproc)}"
steps=("$@"); [[ ${#steps[@]} -gt 0 ]] || steps=(host deps webkit browser cpio)
mkdir -p "$B" "$A/out"

for step in "${steps[@]}"; do
  case "$step" in
  host) bash "$A/tools/host-tools.sh" ;;
  deps) bash "$A/tools/deps.sh" ;;
  webkit)
    if stage webkit-src "$B/src/webkit/.git"; then FORCE=webkit-src bash "$A/tools/webkit.sh" src; fi
    [[ -f "$B/webkit/build.ninja" ]] || bash "$A/tools/webkit.sh" configure
    JOBS="${WEBKIT_JOBS:-4}" bash "$A/tools/webkit.sh" build ;;
  browser) bash "$A/tools/browser.sh" ;;
  cpio)
    CPIO="$A/out/wpe.cpio"
    F="$B/fonts"
    if stage fonts "$F/.stamp"; then
      rm -rf "$F"; mkdir -p "$F/dejavu" "$F/nanum"
      t="$(mktemp -d)"; unpack dejavu "$t"
      # the faces pages ask for most; the rest of DejaVu (condensed, extra light) left out
      for f in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique DejaVuSerif DejaVuSerif-Bold \
               DejaVuSerif-Italic DejaVuSerif-BoldItalic DejaVuSansMono DejaVuSansMono-Bold; do
        cp "$t/ttf/$f.ttf" "$F/dejavu/"
      done
      cp "$t/LICENSE" "$F/dejavu/LICENSE"; rm -rf "$t"
      cp "$(fetch nanum-regular)" "$F/nanum/NanumGothic-Regular.ttf"
      cp "$(fetch nanum-bold)" "$F/nanum/NanumGothic-Bold.ttf"
      touch "$F/.stamp"
    fi
    WASM="$B/browser/wpe-browser.wasm"
    FC="$B/deps/fontconfig-dest"
    [[ -f "$WASM" && -d "$FC/etc/fonts" ]] || { echo "build the browser and deps first" >&2; exit 1; }
    if stage cpio "$CPIO" || [[ "$WASM" -nt "$CPIO" || "$B/browser/web.wasm" -nt "$CPIO" || "$A/etc" -nt "$CPIO" \
                                || "$A/share/mime/globs2" -nt "$CPIO" ]]; then
      L="$A/out/licenses"; rm -rf "$L"; mkdir -p "$L"
      S="$B/src/webkit"
      cat "$S/Source/WebCore/LICENSE-LGPL-2.1" > "$L/addon-wpe-webkit-lgpl-2.1.txt"
      cat "$S/Source/WebCore/LICENSE-APPLE" > "$L/addon-wpe-webkit-bsd.txt" 2>/dev/null || true
      cp "$S/Source/ThirdParty/skia/LICENSE" "$L/addon-wpe-skia.txt"
      cp "$F/dejavu/LICENSE" "$L/addon-wpe-dejavu-fonts.txt"
      cp "$(fetch nanum-ofl)" "$L/addon-wpe-nanum-gothic-ofl.txt"
      for d in glib libsoup glib-networking libxml2 harfbuzz freetype fontconfig icu libpsl nghttp2 libpng \
               libjpeg-turbo libwebp brotli pcre2 expat libgcrypt libgpg-error libtasn1 libepoxy libxkbcommon; do
        for lic in COPYING LICENSE LICENSE.txt LICENSE.md COPYING.LIB COPYING.LESSER docs/LICENSE.TXT LICENSES/LGPL-2.1-or-later.txt; do
          [[ -f "$B/deps/$d-src/$lic" ]] && { cp "$B/deps/$d-src/$lic" "$L/addon-wpe-$d.txt"; break; }
        done
      done
      cp "$B/rt-src/libcxx/LICENSE.TXT" "$L/addon-wpe-libcxx.txt"
      python3 "$U/mkinitramfs.py" --overlay -o "$CPIO" \
        --file usr/bin/wpe-browser="$WASM" \
        --link usr/libexec/wpe-webkit-2.0/WPEWebProcess=../../bin/wpe-browser \
        --link usr/libexec/wpe-webkit-2.0/WPENetworkProcess=../../bin/wpe-browser \
        --file usr/bin/web="$B/browser/web.wasm" \
        --tree etc/fonts="$FC/etc/fonts" \
        --tree usr/share/fontconfig="$FC/usr/share/fontconfig" \
        --tree etc/fonts/conf.d="$A/etc/fonts/conf.d" \
        --tree usr/share/fonts/dejavu="$F/dejavu" \
        --tree usr/share/fonts/nanum="$F/nanum" \
        --tree usr/share/mime="$A/share/mime" \
        --dir var/cache/fontconfig
    fi
    ls -l "$CPIO" ;;
  *) echo "unknown stage: $step" >&2; exit 2 ;;
  esac
done
