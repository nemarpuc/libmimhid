# Compiling these examples

These commands build the examples against an extracted release archive. If a
command fails as written, please open an issue.

Each release archive carries both the shared and the static library. The
commands below link the static one, `lib/libmimhid_static.a`.

All examples link against `libmimhid`, which is written in C++ internally. Even
the static library still needs the C++ standard library
at link time, so every command below includes `-lstdc++`. Forgetting it
produces `undefined reference to std::...` linker errors. libmimhid needs no
other library on Linux and macOS; it talks to the board over a serial port.

Run the commands from the package root (the directory that holds `include/` and
`lib/`). In a release archive the examples are installed under
`share/doc/libmimhid/examples/`:

```sh
EX=share/doc/libmimhid/examples

# One example per HID profile kind. None of these opens a board; each builds a
# complete Spec, prints the descriptor it produced, and then prints the exact
# call sequence that would send that profile through a real board.
for p in keyboard mouse toggle gamepad touchscreen touchpad pen digitizer keyboard_battery raw; do
    cc -std=c11 -O2 -Iinclude $EX/c/profiles/$p.c lib/libmimhid_static.a -lstdc++ -lpthread -o $p
done

# Complete sessions. These open the first board they find.
cc -std=c11 -O2 -Iinclude $EX/c/basic_keyboard_mouse.c lib/libmimhid_static.a -lstdc++ -lpthread -o basic_keyboard_mouse
cc -std=c11 -O2 -Iinclude $EX/c/control_transfers.c    lib/libmimhid_static.a -lstdc++ -lpthread -o control_transfers
c++ -std=c++20 -O2 -Iinclude $EX/cpp/multitouch_gesture.cpp lib/libmimhid_static.a -lpthread -o multitouch_gesture

# Programs to watch on a real target (docs/QUICKSTART.md has the walkthrough).
# The ones that use math.h also need -lm.
cc -std=c17 -O2 -Iinclude -I$EX/c/verify $EX/c/verify/verify_keyboard.c lib/libmimhid_static.a -lstdc++ -lpthread -o verify_keyboard
cc -std=c17 -O2 -Iinclude -I$EX/c/verify $EX/c/verify/verify_touch.c    lib/libmimhid_static.a -lstdc++ -lpthread -lm -o verify_touch
cc -std=c17 -O2 -Iinclude -I$EX/c/verify $EX/c/verify/verify_mouse.c    lib/libmimhid_static.a -lstdc++ -lpthread -lm -o verify_mouse
cc -std=c17 -O2 -Iinclude -I$EX/c/verify $EX/c/verify/verify_toggle.c   lib/libmimhid_static.a -lstdc++ -lpthread -o verify_toggle
cc -std=c17 -O2 -Iinclude -I$EX/c/verify $EX/c/verify/verify_all.c      lib/libmimhid_static.a -lstdc++ -lpthread -lm -o verify_all
```

To link the shared library instead (`lib/libmimhid.so`, `lib/libmimhid.dylib`,
or `bin/mimhid.dll`), replace `lib/libmimhid_static.a` with `-Llib -lmimhid` in every command
above, and make sure the shared library is on your loader's search path at run
time (`LD_LIBRARY_PATH=./lib` on Linux, `DYLD_LIBRARY_PATH=./lib` on macOS, or
just keep the `.dll` next to the `.exe` on Windows).

With CMake, `examples/CMakeLists.txt` builds all of them against an installed
package (`find_package(mimhid REQUIRED)` takes the shared library when the
package has one; `COMPONENTS static` asks for the static one), or from a source checkout with
`-DMIMHID_BUILD_EXAMPLES=ON`.

## Python, C# and Rust examples

`examples/python/enumerate.py`, `examples/csharp/Enumerate.csproj` and, in a source checkout, `bindings/rust/mimhid-sys/examples/enumerate.rs` list the boards `mimhid_discover` finds. They need the bindings (`bindings/` in a source checkout, or the `mimhid` wheel and `MimHid` NuGet package) and a shared library of the same version. `mimhid_discover` messages every serial port of the host, so run them only when that is acceptable.

`docs/EXAMPLES.md` in this package explains what each example demonstrates.
