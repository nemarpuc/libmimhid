# Releases

How a release is built, what each release archive contains, the platform
baselines, and the licenses of every distributed file.

## Release workflow

Releases are built by [.github/workflows/release.yml](../.github/workflows/release.yml):

- **Tag release (`vX.Y.Z`).** Pushing a `v*` tag verifies license headers, the
  release tool unit tests, exported symbol lists, version synchronization across
  the tree, and the matching dated section in [CHANGELOG.md](../CHANGELOG.md),
  waits for the `CI` workflow on the tagged commit to succeed, builds all host
  and firmware targets, runs `tools/release/collect_release.py` to verify the
  assembled `dist/` directory, and publishes the GitHub Release (plus PyPI and
  NuGet packages when repository variable `MIMHID_PUBLISH_BINDINGS` is `true`).
- **Dry run (`workflow_dispatch`).** Running the workflow manually with
  `dry_run: true` (the default) builds, packages, and validates every host,
  firmware, SBOM, and binding artifact and uploads `dist/` as the
  `release-assets` workflow artifact without creating a GitHub Release or
  publishing to PyPI or NuGet.

## Release assets

Every GitHub Release contains **exactly 18 uploaded assets**:

### 1. Six host binary archives

| Asset | Platform | Architecture | Baseline |
|---|---|---|---|
| `libmimhid-X.Y.Z-linux-x86_64-ubuntu22.04.tar.gz` | Linux | x86_64 | Ubuntu 22.04 (`glibc 2.35`) |
| `libmimhid-X.Y.Z-linux-aarch64-ubuntu22.04.tar.gz` | Linux | AArch64 | Ubuntu 22.04 (`glibc 2.35`, cross-built) |
| `libmimhid-X.Y.Z-windows-x86_64.zip` | Windows | x86_64 | MSVC dynamic runtime (`/MD`) |
| `libmimhid-X.Y.Z-windows-arm64.zip` | Windows | ARM64 | MSVC dynamic runtime (`/MD`, cross-built) |
| `libmimhid-X.Y.Z-macos-x86_64.tar.gz` | macOS | x86_64 | `MACOSX_DEPLOYMENT_TARGET=11.0`, ad-hoc signed |
| `libmimhid-X.Y.Z-macos-arm64.tar.gz` | macOS | ARM64 | `MACOSX_DEPLOYMENT_TARGET=11.0`, ad-hoc signed |

Each host archive unpacks to a single top-level directory named after the
archive stem and contains both the shared and static libraries in one layout:

- `include/mimhid.h` and `include/mimhid.hpp`
- Shared library (`lib/libmimhid.so.0` + `lib/libmimhid.so` on Linux,
  `lib/libmimhid.0.dylib` + `lib/libmimhid.dylib` on macOS, `bin/mimhid.dll` +
  import library `lib/mimhid.lib` on Windows)
- Static library (`lib/libmimhid_static.a` on Linux and macOS,
  `lib/mimhid_static.lib` on Windows)
- CMake package configuration under `lib/cmake/mimhid/` (`mimhid-config.cmake`,
  `mimhid-config-version.cmake`, `mimhid-shared-targets.cmake`,
  `mimhid-static-targets.cmake`) supporting `COMPONENTS shared` and
  `COMPONENTS static`
- `pkg-config` files under `lib/pkgconfig/` (`mimhid.pc` for the shared library
  and `mimhid-static.pc` for the static library)
- Example sources and build instructions under `share/doc/libmimhid/examples/`
- License and changelog files under `share/doc/libmimhid/` (`LICENSE`, `NOTICE`,
  `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`)

> [!NOTE]
> Releases prior to `0.2.0` published separate `-shared`, `-static`, and
> `-runtime` archives per platform with per-archive `.spdx.json` sidecars.
> Starting in `0.2.0`, every platform has one archive containing both libraries
> and one release-wide SPDX document.

### 2. Eight firmware assets

| Asset | Target board / MCU | Default UART pins in prebuilt image |
|---|---|---|
| `mimhid_firmware-rp2040-X.Y.Z.uf2` | Raspberry Pi Pico (`rp2040`, `PICO_BOARD=pico`) | `uart0`: TX `GP0`, RX `GP1` |
| `mimhid_firmware-rp2350-X.Y.Z.uf2` | Raspberry Pi Pico 2 (`rp2350`, `PICO_BOARD=pico2`) | `uart0`: TX `GP0`, RX `GP1` |
| `mimhid_firmware-nrf52840-X.Y.Z.hex` | Nordic nRF52840 DK (`BOARD=pca10056`) | BSP UART (`P0.06` TX, `P0.08` RX) |
| `mimhid_firmware-nrf52840-X.Y.Z.bin` | Nordic nRF52840 DK (`BOARD=pca10056`) | BSP UART (`P0.06` TX, `P0.08` RX) |
| `mimhid_firmware-stm32f4-X.Y.Z.hex` | WeAct Black Pill V2.0 (`BOARD=stm32f411blackpill`) | BSP `USART2` (`PA2` TX, `PA3` RX) |
| `mimhid_firmware-stm32f4-X.Y.Z.bin` | WeAct Black Pill V2.0 (`BOARD=stm32f411blackpill`) | BSP `USART2` (`PA2` TX, `PA3` RX) |
| `mimhid_firmware-esp32s3-X.Y.Z.bin` | ESP32-S3 (`esp32s3`, merged flash image at `0x0`) | `UART1`: TX `GPIO17`, RX `GPIO18` |
| `THIRD_PARTY_NOTICES.firmware.txt` | License notices and exact SDK commits for the prebuilt firmware binaries | — |

If your board or UART wiring differs from the prebuilt images above, build the
firmware from source with your board and pin definitions
([BUILDING.md](BUILDING.md#building-the-firmware)).

### 3. Four release-wide metadata and source assets

- `libmimhid-X.Y.Z-source.tar.gz` — complete repository source archive for the
  tagged commit.
- `libmimhid-X.Y.Z.spdx.json` — SPDX 2.3 Software Bill of Materials listing the
  source archive and every host archive with its SHA-256 digest and
  `GENERATED_FROM` relationship.
- `release-manifest.json` — machine-readable manifest of the release version,
  commit SHA, and asset digests.
- `SHA256SUMS` — SHA-256 checksums of the release files.

## Platform baselines

- **Linux (`ubuntu22.04`).** Built on Ubuntu 22.04 and checked by
  `tools/release/glibc_check.py` so no symbol requires a `GLIBC` version newer
  than `2.35`. `libmimhid.so` links `-static-libstdc++ -static-libgcc` and hides
  all non-API symbols behind `cmake/mimhid.map` (`MIMHID_0.2`), so the shared
  library does not expose or depend on the host's `libstdc++.so`.
- **macOS (`macos-x86_64`, `macos-arm64`).** Built with
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`, restricted to the public C symbol list
  in `cmake/mimhid.exports`, and signed with an ad-hoc signature
  (`codesign --force --sign -`). Gatekeeper notarization with an Apple Developer
  ID is not applied; if macOS quarantines a downloaded archive, clear the
  attribute with `xattr -dr com.apple.quarantine <unpacked-dir>`.
- **Windows (`windows-x86_64`, `windows-arm64`).** Built with MSVC using the
  dynamic C/C++ runtime (`/MD`). Target machines need the Microsoft Visual C++
  Redistributable (`vcruntime140.dll`, `msvcp140.dll`).

## Licenses

`libmimhid`, its headers, examples, language bindings, and portable firmware
core are licensed under the MIT License ([LICENSE](../LICENSE)). The host
library has no third-party dependencies. Prebuilt firmware binaries link vendor
SDKs and TinyUSB under permissive licenses:

| Component | Where used | License | Notice / source file |
|---|---|---|---|
| `libmimhid` host library, headers, bindings, examples | Host archives, source archive | MIT | [LICENSE](../LICENSE), [NOTICE](../NOTICE) |
| `libmimhid` firmware core and HAL ports (`firmware/`) | Firmware images, source archive | MIT | [LICENSE](../LICENSE) |
| TinyUSB (`hathach/tinyusb`) | All firmware images and `tests/sim` | MIT | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
| Raspberry Pi Pico SDK (`raspberrypi/pico-sdk`) | RP2040 and RP2350 `.uf2` images | BSD-3-Clause | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
| Nordic `nrfx` (`NordicSemiconductor/nrfx`) | nRF52840 `.hex` and `.bin` images | BSD-3-Clause | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
| ARM CMSIS 5 (`ARM-software/CMSIS_5`) | STM32F4 `.hex` and `.bin` images | Apache-2.0 | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
| STMicroelectronics `cmsis_device_f4` | STM32F4 `.hex` and `.bin` images | Apache-2.0 | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
| Espressif ESP-IDF (`espressif/esp-idf`) | ESP32-S3 `.bin` image | Apache-2.0 | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md), `THIRD_PARTY_NOTICES.firmware.txt` |
