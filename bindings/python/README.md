# mimusb Python binding

This wheel contains only the platform-independent `ctypes` declarations for the
libmimusb C ABI. It deliberately contains no `.so`, `.dll`, or `.dylib`. Install
the matching libmimusb shared distribution separately, then pass its exact path
to `mimusb.load(path)`. The binding never guesses a native-library path. The C
header's `*_init` functions are inline and not part of the binary ABI, so a
`ctypes` structure starts all zero: set its `struct_size` and every required
field yourself. A required field left zero is refused with
`MIMUSB_ERR_UNSET_FIELD`.

The Python package version and the native libmimusb version must match. Complete
native archives, dependency notices, and an SPDX SBOM are published with the
corresponding GitHub Release.

The declarations are generated from `include/mimusb.h` by
`tools/bindings/generate.py`; do not edit `mimusb/native.py` by hand.
