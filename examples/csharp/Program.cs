// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// Lists the boards mimusb_discover finds, using only caller-supplied choices.
// mimusb_discover messages every serial port on the host, so run this only when
// that is acceptable. With no board attached it prints nothing and exits 0.
using System.Runtime.InteropServices;
using System.Text;
using MimUsb;

// The board fills a NUL-terminated string in a fixed buffer of this many bytes.
static unsafe string Text(byte* raw, int capacity)
{
    var length = 0;
    while (length < capacity && raw[length] != 0)
        ++length;
    return Encoding.UTF8.GetString(raw, length);
}

if (args.Length != 1 || !uint.TryParse(args[0], out var timeoutMs) || timeoutMs == 0)
{
    Console.Error.WriteLine("usage: Enumerate HANDSHAKE_TIMEOUT_MS");
    return 2;
}

var result = Native.Discover(timeoutMs, out var discovery);
if (result != Result.Ok)
{
    Console.Error.WriteLine(Marshal.PtrToStringUTF8(Native.ResultName(result)));
    return 1;
}
var count = Native.DiscoveryCount(discovery);
for (nuint index = 0; index < count; ++index)
{
    var pointer = Native.DiscoveryGet(discovery, index);
    var board = Marshal.PtrToStructure<DeviceInfo>(pointer);
    unsafe
    {
        Console.WriteLine(
            $"{Text(board.PortName, 64)} firmware {board.VersionMajor}.{board.VersionMinor}." +
            $"{board.VersionPatch} {Text(board.Description, 64)}");
    }
}
Native.DiscoveryDestroy(discovery);
return 0;
