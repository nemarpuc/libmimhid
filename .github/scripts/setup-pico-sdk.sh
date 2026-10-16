#!/usr/bin/env bash
# Fetches the Raspberry Pi Pico SDK at a pinned release and verifies the commits.
set -euo pipefail

dest="${1:?usage: setup-pico-sdk.sh <destination>}"

tag=2.2.0
sdk_sha=a1438dff1d38bd9c65dbd693f0e5db4b9ae91779
tinyusb_sha=86ad6e56c1700e85f1c5678607a762cfe3aa2f47

git clone --depth 1 --branch "$tag" https://github.com/raspberrypi/pico-sdk.git "$dest"
test "$(git -C "$dest" rev-parse HEAD)" = "$sdk_sha"
git -C "$dest" submodule update --init --depth 1 lib/tinyusb
test "$(git -C "$dest/lib/tinyusb" rev-parse HEAD)" = "$tinyusb_sha"
