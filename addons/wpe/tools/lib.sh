# Shared by the wpe add-on's scripts (sourced): downloads checked against sources.lock, logs,
# stages. Expects A (the add-on folder) and B (its build folder).

# fetch NAME: download (once) the archive sources.lock names into build/dl, check its sha256,
# print its path.
fetch() {
  local name="$1" url sum file
  read -r _ url sum < <(grep -E "^$name " "$A/sources.lock") \
    || { echo "fetch: $name is not in sources.lock" >&2; return 1; }
  file="$B/dl/$(basename "$url")"
  mkdir -p "$B/dl"
  if [[ ! -f "$file" ]]; then
    echo "== fetch $(basename "$url")" >&2
    curl -fsSL --retry 3 -o "$file.part" "$url" && mv "$file.part" "$file"
  fi
  echo "$sum  $file" | sha256sum -c --quiet - >&2 \
    || { echo "checksum mismatch: $file (deleted)" >&2; rm -f "$file"; return 1; }
  echo "$file"
}

# unpack NAME DIR: a fresh copy of NAME's source in DIR (the archive's top folder stripped)
unpack() {
  local f
  f="$(fetch "$1")" || return 1
  rm -rf "$2"; mkdir -p "$2"
  case "$f" in
    *.zip) local t; t="$(mktemp -d)"; (cd "$t" && unzip -qDD "$f"); mv "$t"/*/* "$t"/*/.[!.]* "$2"/ 2>/dev/null; rm -rf "$t" ;;
    *.bz2) python3 -c 'import bz2, shutil, sys; shutil.copyfileobj(bz2.open(sys.argv[1]), sys.stdout.buffer)' "$f" \
             | tar -xm -C "$2" --strip-components=1 ;;   # no bzip2 on the build machine
    *) tar -xmf "$f" -C "$2" --strip-components=1 ;;   # -m: archives newer than this clock confuse ninja
  esac
}

# log_run LOG COMMAND...: append the output to LOG; on failure show its end and stop
log_run() {
  local log="$1"; shift
  "$@" >> "$log" 2>&1 || { tail -60 "$log" >&2; echo "failed: $* (log: $log)" >&2; exit 1; }
}

# stage NAME OUTPUT...: true when the stage must run (an output is missing, or FORCE names it)
stage() {
  local name="$1"; shift
  if [[ ",${FORCE:-}," != *",$name,"* ]]; then
    local f ok=1
    for f in "$@"; do [[ -e "$f" ]] || ok=0; done
    if [[ $ok == 1 ]]; then echo "== $name: up to date"; return 1; fi
  fi
  echo "== $name"; return 0
}

# apply_patches DIR PATCH...: apply each existing patch in DIR
apply_patches() {
  local d="$1" p; shift
  for p in "$@"; do
    [[ -f "$p" ]] || continue
    echo "  patch $(basename "$p")" >&2
    patch -d "$d" -p1 --no-backup-if-mismatch -s < "$p" || { echo "patch failed: $p" >&2; exit 1; }
  done
}

# webkit_objects O: the WebKit objects a program links (paths relative to the WebKit build O):
# those CMake links WPEWebProcess with, and the objects of WebKit's object libraries (WTF,
# JavaScriptCore, WebCore), which CMake gives only to what links them directly, so neither the
# static library nor WPEWebProcess's link line carries them; and the injected bundle, a loadable
# module elsewhere (patch 0008 finds it linked in). bmalloc (unused with the system
# malloc) is left to webkit_bmalloc_objects, linked as an archive.
webkit_objects() {
  { awk '/^build bin\/WPEWebProcess:/,/^$/' "$1/build.ninja" | grep -E "^ *LINK_LIBRARIES" \
      | sed 's/^ *LINK_LIBRARIES = //' | tr ' ' '\n' | grep -E '\.o$' | grep -v '^/' | grep -v 'EntryPoint/'
    grep -oE "^build [^:]*CMakeFiles/(WTF|JavaScriptCore|LowLevelInterpreterLib|WebCore|WPEInjectedBundle)\.dir/[^:]+\.o:" \
      "$1/build.ninja" | sed 's/^build //; s/:$//'
    # WebKit's own resources (images, CSS, error pages) register themselves from a constructor:
    # nothing refers to them, so out of the library's archive they would not be linked
    grep -oE "^build [^:]*CMakeFiles/WebKit\.dir/[^:]*/WebKitResourcesGResourceBundle\.c\.o:" "$1/build.ninja" \
      | sed 's/^build //; s/:$//'
  } | sed 's|/\./|/|g' | awk '!seen[$0]++'
}
webkit_bmalloc_objects() {
  grep -oE "^build [^:]*CMakeFiles/bmalloc\.dir/[^:]+\.o:" "$1/build.ninja" | sed 's/^build //; s/:$//'
}
