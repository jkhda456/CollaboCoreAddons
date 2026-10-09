#!/usr/bin/env bash
# Writes the changes made in build/src/webkit (a git repository whose first commit is the pristine
# release) back to patches/webkit/, one patch per topic. tools/webkit.sh src applies them in order.
#
#   tools/save-patches.sh          (then rebuild from a fresh export to check: FORCE=webkit-src)
set -euo pipefail
export LC_ALL=C
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$A/build/src/webkit"
OUT="$A/patches/webkit"
mkdir -p "$OUT"

# topic: paths (git pathspecs) — every changed file must be in exactly one
declare -A TOPICS=(
  [0001-build-for-wasm32]="Source/cmake/OptionsWPE.cmake Source/WTF/wtf/dtoa/utils.h Source/WTF/wtf/simde
      Source/JavaScriptCore/API/glib/JSCOptions.cpp Source/JavaScriptCore/bytecode/ExpressionInfo.cpp Source/JavaScriptCore/runtime/Options.cpp
      Source/ThirdParty/skia/src/core/SkRasterPipeline.h Source/ThirdParty/skia/modules/skcms/src/skcms_internals.h
      Source/ThirdParty/xdgmime/CMakeLists.txt"
  [0002-jsc-collect-from-the-run-loop-on-wasm]="Source/JavaScriptCore/heap/Heap.cpp"
  [0003-shared-memory-between-processes-without-shared-memory]="Source/WebCore/platform/SharedMemory.h
      Source/WebCore/platform/SharedMemory.cpp Source/WebCore/platform/unix/SharedMemoryUnix.cpp
      Source/WebCore/platform/graphics/ShareableBitmap.h Source/WebCore/platform/graphics/ShareableBitmap.cpp
      Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp
      Source/WebKit/UIProcess/wpe/AcceleratedBackingStore.cpp"
  [0004-cpu-rendering-without-egl]="Source/WebKit/WebProcess/glib/WebProcessGLib.cpp
      Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/DrawingAreaCoordinatedGraphicsGLib.cpp"
  [0005-wpe-platform-collabo-display]="Source/WebKit/WPEPlatform/wpe/collabo Source/WebKit/WPEPlatform/CMakeLists.txt
      Source/WebKit/WPEPlatform/wpe/WPEExtensions.cpp Source/WebKit/PlatformWPE.cmake"
  # found by tools/check-fpcasts.py: WebAssembly traps on a call through a pointer of another type
  [0006-callbacks-take-what-they-are-called-with]="Source/WebCore/platform/network/soup/WebKitAutoconfigProxyResolver.cpp
      Source/WebCore/platform/network/soup/WebKitFormDataInputStream.cpp Source/WebKit/Shared/glib/WebKitContextMenuGAction.cpp
      Source/WebKit/UIProcess/API/glib/*PermissionRequest.cpp Source/WebKit/UIProcess/API/glib/WebKitImage.cpp
      Source/WebKit/UIProcess/API/glib/WebKitWebExtension.cpp Source/WebKit/WPEPlatform/wpe/WPEGestureControllerImpl.cpp
      Source/ThirdParty/xdgmime/src/xdgmime.c"
  # the IPC generator: complete argument types before taking a handler's address (wasm signatures)
  [0007-ipc-complete-argument-types-before-taking-handler-addresses]="Source/WebKit/Scripts/webkit/messages.py"
  # processes in a memory-conscious cache model (the browser asks for DocumentBrowser)
  [0009-process-cache-and-prewarming-follow-the-cache-model]="Source/WebKit/UIProcess/WebProcessPool.cpp"
  # no loadable modules: the injected bundle is linked into the program (tools/lib.sh)
  [0008-injected-bundle-linked-in]="Source/WebKit/WebProcess/InjectedBundle/glib/InjectedBundleGlib.cpp"
)

git -C "$S" add -N -- Source/WebKit/WPEPlatform/wpe/collabo   # new files show in the diff
changed="$(git -C "$S" diff --name-only)"
covered=""
rm -f "$OUT"/*.patch
for topic in $(printf '%s\n' "${!TOPICS[@]}" | sort); do
  # shellcheck disable=SC2086
  git -C "$S" diff --no-color -- ${TOPICS[$topic]} > "$OUT/$topic.patch"
  [[ -s "$OUT/$topic.patch" ]] || rm "$OUT/$topic.patch"
  # shellcheck disable=SC2086
  covered+="$(git -C "$S" diff --name-only -- ${TOPICS[$topic]})"$'\n'
done
missing="$(comm -23 <(echo "$changed" | sort -u | sed '/^$/d') <(echo "$covered" | sort -u | sed '/^$/d'))"
if [[ -n "$missing" ]]; then
  echo "changed files in no topic (add them to tools/save-patches.sh):" >&2
  echo "$missing" >&2
  exit 1
fi
ls -l "$OUT"
