#!/usr/bin/env bash
# Build the add-ons: every addons/<name>/build.sh, or only the ones named.
#
#   addons/build.sh              all of them
#   addons/build.sh claude-code  only that one
#
# Each writes addons/<name>/out/<name>.cpio; its addon.json is copied beside it as <name>.json.
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then
  for build in "$A"/*/build.sh; do names+=("$(basename "$(dirname "$build")")"); done
fi
for name in "${names[@]}"; do
  [[ -x "$A/$name/build.sh" ]] || { echo "no add-on $name (addons/$name/build.sh)" >&2; exit 2; }
  echo "==== addon $name"
  "$A/$name/build.sh"
  [[ -f "$A/$name/out/$name.cpio" ]] || { echo "addons/$name/build.sh wrote no out/$name.cpio" >&2; exit 1; }
  if [[ -f "$A/$name/addon.json" ]]; then
    python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$A/$name/addon.json"
    cp "$A/$name/addon.json" "$A/$name/out/$name.json"
  fi
done
