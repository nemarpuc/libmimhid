// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// Links only the externally installed native ABI; this crate does not build or
// configure libmimusb on the caller's behalf.
fn main() {
    if let Ok(directory) = std::env::var("MIMUSB_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
    }
    println!("cargo:rustc-link-lib=dylib=mimusb");
    println!("cargo:rerun-if-env-changed=MIMUSB_LIB_DIR");
}
