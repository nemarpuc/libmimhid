#!/usr/bin/env bash
# Fetches TinyUSB at a pinned release, verifies the commit, and fetches the pinned
# vendor dependencies for the given board family (the commits are fixed by get_deps.py).
set -euo pipefail

dest="${1:?usage: setup-tinyusb.sh <destination> [family]}"
family="${2:-}"

tag=0.18.0
sha=86ad6e56c1700e85f1c5678607a762cfe3aa2f47

git clone --depth 1 --branch "$tag" https://github.com/hathach/tinyusb.git "$dest"
test "$(git -C "$dest" rev-parse HEAD)" = "$sha"
if [ -n "$family" ]; then
    python3 "$dest/tools/get_deps.py" "$family"
fi
