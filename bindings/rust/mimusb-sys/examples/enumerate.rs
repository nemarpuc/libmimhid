// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
//! Lists the boards mimusb_discover finds, using only caller-supplied choices.
//! mimusb_discover messages every serial port on the host, so run this only when
//! that is acceptable. With no board attached it prints nothing and exits 0.

use core::ptr;
use mimusb_sys::*;
use std::ffi::CStr;

fn text(raw: &[core::ffi::c_char]) -> String {
    unsafe { CStr::from_ptr(raw.as_ptr()) }.to_string_lossy().into_owned()
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    let timeout_ms = arguments.next().and_then(|v| v.parse::<u32>().ok()).filter(|v| *v != 0);
    if arguments.next().is_some() || timeout_ms.is_none() {
        eprintln!("usage: enumerate HANDSHAKE_TIMEOUT_MS");
        std::process::exit(2);
    }
    let mut discovery = ptr::null_mut();
    let result = unsafe { mimusb_discover(timeout_ms.expect("validated above"), &mut discovery) };
    if result != MIMUSB_OK {
        let name = unsafe { CStr::from_ptr(mimusb_result_name(result)) };
        eprintln!("{}", name.to_string_lossy());
        std::process::exit(1);
    }
    for index in 0..unsafe { mimusb_discovery_count(discovery) } {
        let info = unsafe { &*mimusb_discovery_get(discovery, index) };
        println!(
            "{} firmware {}.{}.{} {}",
            text(&info.port_name),
            info.version_major,
            info.version_minor,
            info.version_patch,
            text(&info.description)
        );
    }
    unsafe { mimusb_discovery_destroy(discovery) };
}
