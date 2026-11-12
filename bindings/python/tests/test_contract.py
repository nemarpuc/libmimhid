# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
"""What the Python package promises: a small surface, and no value chosen for the caller.

The layout against the compiled C is test_layout.py, which is generated.
"""

import mimusb
from mimusb import native
from mimusb.native import DeviceConfig, KeyboardOptions


def test_package_exports_only_the_binding_surface():
    assert "load" in mimusb.__all__
    assert "MIMUSB_OK" in mimusb.__all__
    assert not hasattr(mimusb, "c")
    assert not hasattr(mimusb, "os")
    assert not hasattr(mimusb, "Union")


def test_a_new_option_structure_is_all_zero():
    # struct_size and every choice are the caller's; the binding fills nothing.
    for structure in (KeyboardOptions, DeviceConfig):
        instance = structure()
        for name, *_ in structure._fields_:
            value = getattr(instance, name)
            assert not isinstance(value, int) or value == 0, name


def test_device_config_exposes_the_serial_and_usb_choices():
    names = {name for name, *_ in DeviceConfig._fields_}
    assert {"struct_size", "transport", "usb_device", "usb_config"} <= names
    transport = {name for name, *_ in native.TransportOptions._fields_}
    assert {"baud_rate", "handshake_timeout_ms", "maximum_report_bytes", "linux_hid",
            "validate_reports", "keep_attached", "event_sink"} <= transport


if __name__ == "__main__":
    test_package_exports_only_the_binding_surface()
    test_a_new_option_structure_is_all_zero()
    test_device_config_exposes_the_serial_and_usb_choices()
