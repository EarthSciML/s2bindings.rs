//! Build script for `s2bindings-sys`.
//!
//! Drives a CMake "superbuild" that compiles the vendored C++ stack
//! (`abseil-cpp` -> `s2geometry`) and a small FFI shim into a single
//! self-contained shared library (`libs2bindings_shim`). Building the C++
//! statically and absorbing it into one shared object keeps the Rust link
//! line trivial: we only link `-ls2bindings_shim` and let its `NEEDED`
//! entries (OpenSSL, libstdc++, libm) resolve from the system at run time.

use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let csrc = manifest_dir.join("csrc");
    let vendor = manifest_dir.join("vendor");

    // Rebuild when the shim or its CMake description changes. (Cargo watches
    // build.rs itself automatically.)
    for f in ["shim.cc", "shim.h", "CMakeLists.txt"] {
        println!("cargo:rerun-if-changed={}", csrc.join(f).display());
    }

    // Fail fast with an actionable message if the vendored C++ submodules were
    // not checked out -- the most common fresh-clone footgun for a -sys crate.
    for marker in ["abseil-cpp/CMakeLists.txt", "s2geometry/CMakeLists.txt"] {
        let p = vendor.join(marker);
        assert!(
            p.exists(),
            "vendored submodule missing: {}\n\
             Initialize submodules before building:\n    \
             git submodule update --init --recursive",
            p.display()
        );
    }

    // Configure + build the superbuild under OUT_DIR (which lives inside
    // target/), keeping the heavy C++ build artifacts off the inode-tight home
    // filesystem. We build only the `s2bindings_shim` target -- CMake pulls in
    // exactly the abseil + s2geometry pieces it needs as link dependencies.
    //
    // We force a Release build of the vendored C++ regardless of the Rust
    // profile: a debug build of abseil + s2geometry is large and slow, and the
    // FFI surface does not benefit from C++ debug symbols.
    let dst = cmake::Config::new(&csrc)
        .define("VENDOR_DIR", vendor.to_str().unwrap())
        .profile("Release")
        .build_target("s2bindings_shim")
        .build();

    // The shim shared library is produced in the CMake build directory
    // (install rules are intentionally skipped; see csrc/CMakeLists.txt).
    let build_dir = dst.join("build");
    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=dylib=s2bindings_shim");

    // Embed an rpath so the shim shared library is found at run time when
    // tests / examples / downstream binaries run straight out of target/.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", build_dir.display());
}
