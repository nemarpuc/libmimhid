// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Links only the externally installed native ABI; this crate does not build or
// configure libmimhid on the caller's behalf.
fn main() {
    if let Ok(directory) = std::env::var("MIMHID_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
    }
    println!("cargo:rustc-link-lib=dylib=mimhid");
    println!("cargo:rerun-if-env-changed=MIMHID_LIB_DIR");
}
