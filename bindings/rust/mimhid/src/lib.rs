// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
//! Typed ownership/reference wrappers over `mimhid-sys`.
//!
//! The option structures are those of `mimhid-sys`, filled in by the caller. The
//! initial values the C header's `*_init` functions use are listed in docs/API.md.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ptr::NonNull;
use mimhid_sys as sys;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Error(pub sys::mimhid_result);

fn result(value: sys::mimhid_result) -> Result<(), Error> {
    if value == sys::MIMHID_OK {
        Ok(())
    } else {
        Err(Error(value))
    }
}

macro_rules! safe_spec_type {
    ($name:ident, $options:ty, $factory:path) => {
        pub struct $name(NonNull<sys::mimhid_spec>);

        impl $name {
            pub fn create(options: &$options) -> Result<Self, Error> {
                let mut output = core::ptr::null_mut();
                let status = unsafe { $factory(options, &mut output) };
                if status != sys::MIMHID_OK {
                    return Err(Error(status));
                }
                NonNull::new(output)
                    .map(Self)
                    .ok_or(Error(sys::MIMHID_ERR_INTERNAL))
            }

            pub fn as_ptr(&self) -> *mut sys::mimhid_spec {
                self.0.as_ptr()
            }
        }

        impl Drop for $name {
            fn drop(&mut self) {
                unsafe { sys::mimhid_spec_release(self.0.as_ptr()) }
            }
        }
    };
}

macro_rules! pointer_spec_type {
    ($name:ident, $options:ty, $factory:path) => {
        pub struct $name(NonNull<sys::mimhid_spec>);

        impl $name {
            /// # Safety
            /// Every pointer/count pair and nested C string in `options` must
            /// identify readable storage for the duration of this call.
            pub unsafe fn create(options: &$options) -> Result<Self, Error> {
                let mut output = core::ptr::null_mut();
                let status = unsafe { $factory(options, &mut output) };
                if status != sys::MIMHID_OK {
                    return Err(Error(status));
                }
                NonNull::new(output)
                    .map(Self)
                    .ok_or(Error(sys::MIMHID_ERR_INTERNAL))
            }

            pub fn as_ptr(&self) -> *mut sys::mimhid_spec {
                self.0.as_ptr()
            }
        }

        impl Drop for $name {
            fn drop(&mut self) {
                unsafe { sys::mimhid_spec_release(self.0.as_ptr()) }
            }
        }
    };
}

safe_spec_type!(
    KeyboardSpec,
    sys::mimhid_keyboard_options,
    sys::mimhid_spec_create_keyboard
);
safe_spec_type!(
    MouseSpec,
    sys::mimhid_mouse_options,
    sys::mimhid_spec_create_mouse
);
pointer_spec_type!(
    ToggleSpec,
    sys::mimhid_toggle_options,
    sys::mimhid_spec_create_toggle
);
pointer_spec_type!(
    GamepadSpec,
    sys::mimhid_gamepad_options,
    sys::mimhid_spec_create_gamepad
);
safe_spec_type!(
    TouchscreenSpec,
    sys::mimhid_touchscreen_options,
    sys::mimhid_spec_create_touchscreen
);
safe_spec_type!(
    TouchpadSpec,
    sys::mimhid_touchpad_options,
    sys::mimhid_spec_create_touchpad
);
pointer_spec_type!(
    PenSpec,
    sys::mimhid_pen_options,
    sys::mimhid_spec_create_pen
);
pointer_spec_type!(
    DigitizerSpec,
    sys::mimhid_digitizer_options,
    sys::mimhid_spec_create_digitizer
);
pointer_spec_type!(
    RawSpec,
    sys::mimhid_raw_options,
    sys::mimhid_spec_create_raw
);

#[derive(Clone, Copy)]
pub struct NodeRef(NonNull<sys::mimhid_node>);

impl NodeRef {
    /// # Safety
    /// `node` must stay live for every call made through this non-owning value.
    pub unsafe fn from_raw(node: *mut sys::mimhid_node) -> Option<Self> {
        NonNull::new(node).map(Self)
    }

    pub fn as_ptr(self) -> *mut sys::mimhid_node {
        self.0.as_ptr()
    }

    /// The Node's id on the board (`mimhid_node_id`).
    pub fn id(self) -> u16 {
        unsafe { sys::mimhid_node_id(self.as_ptr()) }
    }

    pub fn submit(self, send_timeout_ms: u32) -> Result<(), Error> {
        result(unsafe { sys::mimhid_node_submit(self.as_ptr(), send_timeout_ms) })
    }

    pub fn submit_blocking(self, timeout_ms: u32) -> Result<(), Error> {
        result(unsafe { sys::mimhid_node_submit_blocking(self.as_ptr(), timeout_ms) })
    }
}

macro_rules! node_type {
    ($name:ident) => {
        #[derive(Clone, Copy)]
        pub struct $name(NodeRef);

        impl $name {
            /// # Safety
            /// `node` must have this wrapper's profile kind and remain live.
            pub unsafe fn from_raw(node: *mut sys::mimhid_node) -> Option<Self> {
                unsafe { NodeRef::from_raw(node) }.map(Self)
            }

            pub fn as_ptr(self) -> *mut sys::mimhid_node {
                self.0.as_ptr()
            }

            pub fn submit(self, send_timeout_ms: u32) -> Result<(), Error> {
                self.0.submit(send_timeout_ms)
            }

            pub fn submit_blocking(self, timeout_ms: u32) -> Result<(), Error> {
                self.0.submit_blocking(timeout_ms)
            }

            /// Sets the battery level; the Spec must have a battery report.
            pub fn battery(self, strength: i32) -> Result<(), Error> {
                result(unsafe { sys::mimhid_battery_update(self.as_ptr(), strength) })
            }
        }
    };
}

node_type!(KeyboardNodeRef);
node_type!(MouseNodeRef);
node_type!(ToggleNodeRef);
node_type!(GamepadNodeRef);
node_type!(TouchscreenNodeRef);
node_type!(TouchpadNodeRef);
node_type!(PenNodeRef);
node_type!(DigitizerNodeRef);
node_type!(RawNodeRef);

impl KeyboardNodeRef {
    pub fn key(self, usage: u16, down: bool) -> Result<(), Error> {
        result(unsafe { sys::mimhid_keyboard_key(self.as_ptr(), usage, u32::from(down)) })
    }
}

impl MouseNodeRef {
    pub fn move_by(self, dx: i32, dy: i32) -> Result<(), Error> {
        result(unsafe { sys::mimhid_mouse_move(self.as_ptr(), dx, dy) })
    }
    pub fn scroll(self, wheel: i32, pan: i32) -> Result<(), Error> {
        result(unsafe { sys::mimhid_mouse_scroll(self.as_ptr(), wheel, pan) })
    }
    pub fn button(self, button: u32, down: bool) -> Result<(), Error> {
        result(unsafe { sys::mimhid_mouse_button(self.as_ptr(), button, u32::from(down)) })
    }
}

/// Covers every allow-listed one-bit selector page: Consumer, System Control,
/// Camera keys, Telephony keys, or a caller-chosen HUT page. Which page a
/// bound node speaks is decided entirely by the Spec it was opened with.
impl ToggleNodeRef {
    pub fn set(self, usage: u16, down: bool) -> Result<(), Error> {
        result(unsafe { sys::mimhid_toggle_usage(self.as_ptr(), usage, u32::from(down)) })
    }
}

impl GamepadNodeRef {
    pub fn button(self, button: u32, down: bool) -> Result<(), Error> {
        result(unsafe { sys::mimhid_gamepad_button(self.as_ptr(), button, u32::from(down)) })
    }
    pub fn axis(self, axis_index: usize, value: i32) -> Result<(), Error> {
        result(unsafe { sys::mimhid_gamepad_axis(self.as_ptr(), axis_index, value) })
    }
    pub fn dpad(self, up: bool, down: bool, right: bool, left: bool) -> Result<(), Error> {
        result(unsafe {
            sys::mimhid_gamepad_dpad(
                self.as_ptr(),
                u32::from(up),
                u32::from(down),
                u32::from(right),
                u32::from(left),
            )
        })
    }
}

/// Always fixed-slot Multi-Touch.
impl TouchscreenNodeRef {
    pub fn contact(
        self,
        contact_id: u32,
        down: bool,
        x: i32,
        y: i32,
        extra: Option<&sys::mimhid_touchscreen_extra>,
    ) -> Result<(), Error> {
        result(unsafe {
            sys::mimhid_touchscreen_contact(
                self.as_ptr(),
                contact_id,
                u32::from(down),
                x,
                y,
                extra.map_or(core::ptr::null(), |value| value as *const _),
            )
        })
    }
}

/// Always fixed-slot Multi-Touch under the Touch Pad Application Collection.
impl TouchpadNodeRef {
    pub fn contact(
        self,
        contact_id: u32,
        down: bool,
        x: i32,
        y: i32,
        extra: Option<&sys::mimhid_touchpad_extra>,
    ) -> Result<(), Error> {
        result(unsafe {
            sys::mimhid_touchpad_contact(
                self.as_ptr(),
                contact_id,
                u32::from(down),
                x,
                y,
                extra.map_or(core::ptr::null(), |value| value as *const _),
            )
        })
    }

    pub fn button(self, button: u32, down: bool) -> Result<(), Error> {
        result(unsafe { sys::mimhid_touchpad_button(self.as_ptr(), button, u32::from(down)) })
    }
}

impl PenNodeRef {
    pub fn update(self, sample: &sys::mimhid_pen_sample) -> Result<(), Error> {
        result(unsafe { sys::mimhid_pen_update(self.as_ptr(), sample) })
    }
}

/// Same stylus report shape as `PenNodeRef`, under the Digitizer Application
/// Collection.
impl DigitizerNodeRef {
    pub fn update(self, sample: &sys::mimhid_digitizer_sample) -> Result<(), Error> {
        result(unsafe { sys::mimhid_digitizer_update(self.as_ptr(), sample) })
    }
}

impl RawNodeRef {
    pub fn report(self, bytes: &[u8], send_timeout_ms: u32) -> Result<(), Error> {
        result(unsafe {
            sys::mimhid_raw_submit(self.as_ptr(), bytes.as_ptr(), bytes.len(), send_timeout_ms)
        })
    }
}

/// A Node opened with `mimhid_node_open`: interfaces of the caller's own, which may be HID written
/// by hand. It has no Spec; its calls are the endpoint calls, of which `write` is the one that
/// sends a transfer.
#[derive(Clone, Copy)]
pub struct GenericNodeRef(NodeRef);

impl GenericNodeRef {
    /// # Safety
    /// `node` must come from `mimhid_node_open` and remain live.
    pub unsafe fn from_raw(node: *mut sys::mimhid_node) -> Option<Self> {
        unsafe { NodeRef::from_raw(node) }.map(Self)
    }

    pub fn as_ptr(self) -> *mut sys::mimhid_node {
        self.0.as_ptr()
    }

    pub fn id(self) -> u16 {
        self.0.id()
    }

    /// Writes `bytes` to the IN endpoint `address` of the Node as one transfer; for a HID
    /// interface, one report with its Report ID, if it has them.
    pub fn write(self, address: u8, bytes: &[u8], timeout_ms: u32) -> Result<(), Error> {
        result(unsafe {
            sys::mimhid_node_endpoint_write(
                self.as_ptr(),
                address,
                bytes.as_ptr(),
                bytes.len(),
                timeout_ms,
            )
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn error_preserves_native_code() {
        assert_eq!(Error(9), Error(9));
    }

    fn keyboard_options() -> sys::mimhid_keyboard_options {
        // SAFETY: the structure is plain data; all-zero is the documented unset state.
        let mut options: sys::mimhid_keyboard_options = unsafe { core::mem::zeroed() };
        options.struct_size = core::mem::size_of::<sys::mimhid_keyboard_options>() as u32;
        options
    }

    #[test]
    fn an_unset_range_is_refused_with_the_native_code() {
        // No usage range was given, and nothing is guessed: the range is refused, not defaulted.
        let error = KeyboardSpec::create(&keyboard_options())
            .err()
            .expect("must be refused");
        assert_eq!(error, Error(sys::MIMHID_ERR_PARAM));
    }

    #[test]
    fn a_spec_is_created_and_released() {
        let mut options = keyboard_options();
        options.usage_minimum = 0x04;
        options.usage_maximum = 0xDD;
        let spec = KeyboardSpec::create(&options).expect("valid keyboard");
        assert!(!spec.as_ptr().is_null());
    }
}
