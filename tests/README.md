# Tests

libmimhid is tested in four layers. Each one shows something the others cannot, and each says
plainly what it does not show.

| Layer | Runs on | Shows | Does not show |
|---|---|---|---|
| **unit** | any host, no board | the HID layer, the profile state machines, the ABI's argument checks, framing | anything that crosses the serial port |
| **scripted** | Linux and macOS, no board | the host transport and C ABI against a programmable pseudo-terminal peer that injects NAKs, corrupt frames, rate refusals, and hang-ups | the real firmware core, TinyUSB, or real hardware |
| **sim** | Linux and macOS, no board | the whole host library talking to the real firmware core (`firmware/common`) over a pseudo-terminal | USB, real serial adapters, real target operating systems |
| **hw** | a real board, adapter and target | that a release works on hardware | anything not on the bench that day |

The names are kept strictly: `unit` is in-process, `scripted` is the programmable peer, `sim` is the
simulated board running the real firmware core, and `hw` means real hardware and nothing else.
[`hw/README.md`](hw/README.md) is the procedure for the hardware layer.

## Directories

| Directory | ctest name | What it guards, and why |
|---|---|---|
| `unit/` | `mimhid.unit` | One file per area (`test_<suite>.cpp`). Links the library's internal objects, so it can test parsers, builders and per-node state without a device. `test_main.cpp` lists the suites. |
| `scripted/` | `mimhid.scripted` | A board the test plays itself on a pseudo-terminal, frame by frame (`scripted_board.*`). It can NAK, stay silent, refuse a rate, stop reading, hang up, or send broken frames: the error paths a working firmware rarely takes. POSIX only; no TinyUSB. |
| `sim/` | `mimhid.sim-15`, `-7`, `-3`, `-hs` | The real firmware core (`firmware/common`) built for the host against TinyUSB, driven over a pseudo-terminal (`fake_board.c`). Four builds with 15, 7 and 3 HID interfaces and high speed. Needs `MIMHID_TINYUSB_PATH`. |
| `abi/` | `mimhid.abi-layout-oracle`, `mimhid.c-abi`, `mimhid.cpp-wrapper` | The C ABI's structure layouts against `abi_layout.golden`, a C11 consumer, and the C++ header-only wrapper. Any change to a public structure shows up here first. |
| `golden/` | `mimhid.golden-descriptors` | Every profile's report descriptor, byte for byte (`tests/hidtools/export_descriptors.cpp` writes them). A descriptor change has to be deliberate. CI also parses them with the official hid-tools parser. |
| `fuzz/` | (CI job `fuzz-smoke`) | libFuzzer on the raw descriptor validator. Clang only; `MIMHID_BUILD_FUZZ=ON`. |
| `cmake/` | `mimhid.package-*` | The installed CMake package: components and the multi-architecture layout. |
| `release/` | (CI, `python3 tests/release/test_release_tools.py`) | The release tooling in `tools/release`. |
| `docs/` | (CI, `python3 tests/docs/test_doc_references.py`) | `tools/check_doc_references.py`, which fails when a document names a `mimhid_*` / `MIMHID_*` / `mimhid::*` identifier the code does not have, or a path that does not exist; deliberate exceptions go in `tools/doc_reference_allowlist.txt`, each with its reason. |
| `hw/` | none | The hardware procedure. Nothing here runs by itself. |

The firmware core also has its own host-side tests in `firmware/tests` (CI job "Firmware core tests").

Every runner counts the checks it ran and prints `N checks, M failed`, and every runner of suites
takes suite names as arguments (`--list` prints them):

```sh
build/dev/mimhid_tests --list
build/dev/mimhid_tests profiles_touch spec_rejections
build/dev/mimhid_scripted_tests handshaker
```

## Running them

All commands run from the repository root and build into `build/` there (ignored by git).

```sh
# Unit, scripted, ABI, golden and package tests, plus sim when TinyUSB is given
cmake --preset dev -B build/dev -DMIMHID_TINYUSB_PATH=$HOME/tinyusb
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure

# One test, or one layer
ctest --test-dir build/dev -R mimhid.unit --output-on-failure
ctest --test-dir build/dev -R sim --output-on-failure

# AddressSanitizer and UndefinedBehaviorSanitizer
cmake -B build/asan -S . -DCMAKE_BUILD_TYPE=Debug -DMIMHID_BUILD_TESTS=ON \
      -DMIMHID_SANITIZE=ON -DMIMHID_TINYUSB_PATH=$HOME/tinyusb
cmake --build build/asan && ctest --test-dir build/asan

# ThreadSanitizer (tests/sim/tsan.supp holds the suppressions)
cmake --preset tsan -B build/tsan -DMIMHID_TINYUSB_PATH=$HOME/tinyusb
cmake --build build/tsan && ctest --test-dir build/tsan
```

`.github/scripts/setup-tinyusb.sh <dir>` fetches the pinned TinyUSB that CI uses.

## Coverage

`MIMHID_COVERAGE=ON` (the `coverage` preset) builds everything with `--coverage -O0`. GCC and Clang
only; it cannot be combined with the sanitizers or the fuzz build. The CI job "Coverage" runs the
same commands and fails when line or branch coverage of `src/` drops under its floor.

```sh
cmake --preset coverage -B build/coverage -DMIMHID_TINYUSB_PATH=$HOME/tinyusb
cmake --build build/coverage
find build/coverage -name '*.gcda' -delete   # counts from an earlier run
ctest --test-dir build/coverage
mkdir -p build/coverage/coverage
gcovr --root . --object-directory build/coverage --filter 'src/' \
  --exclude-throw-branches --exclude-unreachable-branches \
  --exclude-lines-by-pattern '.*MIMHID_C_(RESULT|VALUE|VOID)_CATCH.*' \
  --txt build/coverage/coverage/coverage.txt \
  --html-details build/coverage/coverage/index.html \
  --print-summary --fail-under-line 86 --fail-under-branch 73
```

gcovr 8.8 is what CI installs (`pipx install gcovr==8.8`).

What is left out of the figures on purpose:

- **The catch macros** (`MIMHID_C_*_CATCH`) at the end of every C entry point. They turn a C++
  exception into `MIMHID_ERR_INTERNAL`; nothing short of fault injection reaches them.
- **Throw and unreachable branches**, which gcc emits for every call that may throw.
- **The Windows and macOS files** (`serial_win32.cpp`, `ports_win32.cpp`, `ports_macos.cpp`) are not
  built on Linux, so they do not appear at all. Their tests run
  in the CI jobs on those systems, without coverage.
- **`mimhid_discover`'s search of real ports**: it messages every serial port on the machine, so
  the tests call it only with arguments it refuses. The port listing it uses is tested against a
  made-up sysfs tree (`scripted/test_ports_linux.cpp`).

What remains uncovered is mostly defensive: checks that a generated descriptor matches its own
layout (`MIMHID_ERR_INTERNAL` in `validator.cpp` and `spec.cpp`), allocation failures, and OS errors
from `tcsetattr` and friends that a pseudo-terminal does not produce.

## Adding a test

- A new behaviour of a profile, the validator or the builder: a check in the matching
  `unit/test_<area>.cpp`. A new area: a new file with `void test_<suite>()`, declared in `test.hpp`,
  listed in `unit/test_main.cpp` and in `cmake/mimhid-tests.cmake`.
- A refusal: start from options the call accepts and change one field, as `test_spec_rejections`
  and `test_input_rejections` do, and check both the result and `mimhid_last_error()->field`.
- How the library reacts to the board misbehaving: `scripted/`. `Board::set_policy` answers any
  command as you like (`Reply::ack`, `Reply::nak`, `Reply::silent`); `set_pong`, `set_carries`,
  `set_reading`, `hang_up` and `send_event` cover the rest.
- How the library and the firmware work together: `sim/`. These run against the real firmware core,
  so a test there shows both sides agree.
- Something only hardware can show: write it down in `hw/README.md`, not as a test that passes
  without the hardware.
- Every check is `MIMHID_CHECK(expression)` from `test.hpp`; every new file carries the SPDX header
  (`python3 tools/check_license_headers.py`), and `clang-format` 14 decides the layout.
