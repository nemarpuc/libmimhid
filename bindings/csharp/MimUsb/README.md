# MimUsb .NET binding

This NuGet package contains only the managed P/Invoke declarations for the
libmimusb C ABI. It does not contain `mimusb.dll` or `libmimusb.so`. Install the
matching libmimusb shared distribution separately and make it available to the
.NET native-library loader under the name `mimusb`.

The NuGet package version and native libmimusb version must match. Complete
native archives, dependency notices, and SPDX documents are published with the
corresponding GitHub Release. The C header's `*_init` functions are inline and
not part of the binary ABI, so a structure starts all zero: set its
`StructSize` and every required field yourself. A required field left zero is
refused with `Result.UnsetField`.

## Callback lifetime

`DeviceConfig.Transport.EventSink` and `LogSink` hold native function pointers,
typically obtained with `Marshal.GetFunctionPointerForDelegate(sink)`. The
garbage collector does not know the native side holds them: keep the delegate
referenced for as long as the Device is open.

## Pointers

Functions that take a pointer to a structure have two forms: one with `in` or
`ref` for the common case, and one taking `nint` where `null` is allowed
(`nint.Zero`). The declarations are generated from `include/mimusb.h` by
`tools/bindings/generate.py`; do not edit `Native.cs` by hand.
