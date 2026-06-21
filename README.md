# s2bindings.rs

Rust FFI bindings over Google's [s2geometry] spherical-geometry engine — the
same C++ core that backs Python's [spherely] and R's [s2]. The goal is a thin,
safe Rust API for **spherical** polygon operations (clipping / intersection and
area) that Rust's planar geometry crates cannot provide correctly.

> **Status: build scaffold.** This first milestone stands up the crate, the
> vendored C++ build, CI, and an FFI smoke test that proves the C++ stack links
> and runs. The spherical geometry kernel (`intersect_polygon`, area) and its
> safe wrappers land in a follow-up.

[s2geometry]: https://github.com/google/s2geometry
[spherely]: https://github.com/benbovy/spherely
[s2]: https://github.com/r-spatial/s2

## Layout

```
crates/s2bindings-sys/      Low-level -sys crate (FFI + native build)
  build.rs                  Drives the CMake superbuild, links the shim
  csrc/                     C ABI shim + CMake superbuild
    CMakeLists.txt          abseil -> s2geometry -> shared shim
    shim.h / shim.cc        extern "C" surface over s2geometry
  src/lib.rs                Rust `extern "C"` declarations + smoke API
  vendor/                   Vendored C++ sources (git submodules)
    abseil-cpp/             pinned 20240116.2
    s2geometry/             pinned v0.11.1
```

## How the native build works

`s2bindings-sys` links the C++ stack via a CMake **superbuild** invoked from
`build.rs`:

1. **abseil-cpp** and **s2geometry** are vendored as git submodules and built
   from source (static, position-independent). Because s2geometry guards its
   dependency lookup with `if(NOT TARGET absl::base)`, the superbuild's
   `add_subdirectory(abseil-cpp)` supplies abseil directly — **no system
   abseil, and therefore no version-skew risk** between abseil and s2geometry.
2. A small C ABI shim (`csrc/shim.cc`) is compiled into a single self-contained
   **shared library** (`libs2bindings_shim`) that statically absorbs abseil and
   s2geometry. The Rust link line then only needs `-ls2bindings_shim`.
3. All build artifacts land under `target/` (Cargo's `OUT_DIR`), keeping the
   heavy C++ build off the home filesystem.

Only **OpenSSL** is required from the system (s2geometry depends on it);
everything else is vendored.

## Building

### Prerequisites

- A C++17 compiler (GCC ≥ 7 or Clang)
- [CMake] ≥ 3.16
- OpenSSL development headers
- A Rust toolchain (stable)

[CMake]: https://cmake.org/

#### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libssl-dev
```

#### Fedora / RHEL

```bash
sudo dnf install -y gcc-c++ cmake openssl-devel
```

#### macOS (Homebrew)

```bash
brew install cmake openssl
```

### Clone (with submodules) and build

The C++ sources are git submodules, so clone recursively (or initialize the
submodules after a plain clone):

```bash
git clone --recurse-submodules https://github.com/EarthSciML/s2bindings.rs.git
# or, after a non-recursive clone:
git submodule update --init --recursive

cargo build
cargo test      # runs the FFI smoke test
```

The first build compiles abseil + s2geometry from source and takes several
minutes; subsequent builds are incremental.

## Smoke test

The scaffold exposes a minimal identity surface (in `s2bindings-sys`) used only
to prove the link works:

- `s2_max_level()` → `30` (`S2CellId::kMaxLevel`), an identity/version check
  that the s2geometry headers compiled and the library linked.
- `unit_point_norm(lat, lng)` → `1.0`, which runs real s2geometry math
  (`S2LatLng::FromDegrees(...).ToPoint()`) to confirm the linked library
  executes, not merely links.

`cargo test` exercises both.

## Continuous integration

`.github/workflows/ci.yml` runs on Ubuntu: it checks out submodules
recursively, installs the OpenSSL toolchain via `apt`, then runs
`cargo fmt --check`, `cargo build`, `cargo test` (the smoke test), and
`cargo clippy`. CI is the canonical build gate.

## Scope notes

- **s2geography is intentionally not built yet.** The target kernel —
  `intersect_polygon` (`S2Polygon::InitToIntersection`) and spherical area
  (`S2Polygon::GetArea`) — lives in **s2geometry** itself, so s2geography is not
  required for it. s2geography 0.3.0 additionally pulls in a build-time
  `nanoarrow` fetch and a mandatory installed-abseil lookup, neither of which is
  needed here; it can be added later if WKT/WKB/GeoArrow I/O is wanted.
- **Vendoring choice.** abseil + s2geometry are pinned git submodules rather
  than system packages because s2geometry is not apt-installable and abseil's
  ABI is sensitive to the exact version and C++ standard it is built with.
  Vendoring makes the build hermetic and reproducible across the (RHEL) cluster
  and (Ubuntu) CI.

## License

MIT — see [LICENSE](LICENSE).
