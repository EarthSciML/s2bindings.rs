# s2bindings.rs

Rust FFI bindings over Google's [s2geometry] spherical-geometry engine — the
same C++ core that backs Python's [spherely] and R's [s2]. It provides a thin,
safe Rust API for **spherical** polygon operations — intersection (clipping)
and area — that Rust's planar geometry crates cannot compute correctly for
lon/lat data spanning large areas or near the poles / antimeridian.

> **Status: v0.1.0.** The spherical kernel — `SphericalPolygon::intersection`
> (`S2Polygon::InitToIntersection`) and `SphericalPolygon::area`
> (`S2Polygon::GetArea`) — is implemented with safe Rust wrappers and tests on
> top of the vendored C++ build. Pin it as a git/tag dependency (see
> [Depending on this crate](#depending-on-this-crate)).

[s2geometry]: https://github.com/google/s2geometry
[spherely]: https://github.com/benbovy/spherely
[s2]: https://github.com/r-spatial/s2

## Layout

```
crates/s2bindings/          Safe, idiomatic Rust API (depend on THIS crate)
  src/lib.rs                SphericalPolygon: from_lon_lat / intersection / area / rings
crates/s2bindings-sys/      Low-level -sys crate (raw FFI + native build)
  build.rs                  Drives the CMake superbuild, links the shim
  csrc/                     C ABI shim + CMake superbuild
    CMakeLists.txt          abseil -> s2geometry -> shared shim
    shim.h / shim.cc        extern "C" surface over s2geometry
  src/lib.rs                Rust `extern "C"` declarations (raw, unsafe)
  vendor/                   Vendored C++ sources (git submodules)
    abseil-cpp/             pinned 20240116.2
    s2geometry/             pinned v0.11.1
```

`s2bindings` is the public, safe API. `s2bindings-sys` exposes the raw,
`unsafe` C ABI and is an implementation detail — depend on `s2bindings`.

## Using the library

```rust
use s2bindings::{S2Error, SphericalPolygon};

fn clip_example() -> Result<(), S2Error> {
    // Vertices are (longitude, latitude) in degrees; rings are implicitly closed.
    let a = SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (90.0, 0.0), (0.0, 90.0)])?;
    let b = SphericalPolygon::from_lon_lat(&[(45.0, 0.0), (135.0, 0.0), (45.0, 90.0)])?;

    // Spherical area, in steradians (unit sphere).
    let _area_sr = a.area(); // π/2 for this octant
    let _area_m2 = a.area_on_sphere(6_371_008.8); // scaled to Earth's mean radius

    // Great-circle intersection (clip). May be empty for disjoint / tangent inputs.
    let clip = a.intersection(&b)?;
    if !clip.is_empty() {
        for ring in clip.rings() {
            // ring.vertices: Vec<(lon, lat)>; ring.is_hole: bool
            let _ = ring;
        }
    }
    Ok(())
}
```

### API surface

| Method | Description |
|--------|-------------|
| `SphericalPolygon::from_lon_lat(&[(f64, f64)])` | Build a polygon from one great-circle loop of `(lon, lat)` degrees. `Err` on a degenerate/invalid loop. |
| `.area() -> f64` | Enclosed area in **steradians** (unit sphere), range `[0, 4π]`. |
| `.area_on_sphere(radius) -> f64` | `area()` scaled by `radius²` (physical area). |
| `.intersection(&other) -> Result<SphericalPolygon, S2Error>` | Great-circle intersection / clip. |
| `.is_empty() -> bool` | `true` if the polygon encloses no area. |
| `.num_loops() -> usize` | Loop count (0 empty, 1 simple, more with holes / pieces). |
| `.rings() -> Vec<Ring>` | Boundary loops; each `Ring` has `vertices: Vec<(lon, lat)>` and `is_hole: bool`. |

## Geometry model (the manifold / edge contract)

- **Coordinates** are `(longitude, latitude)` pairs in **degrees**
  (`x = lon`, `y = lat`) — the order used by GeoJSON and `GeometryOps`.
- **Edges are geodesics** (great-circle arcs) between consecutive vertices.
  This is the crucial difference from planar clipping: the edge from
  `(0°, 10°)` to `(20°, 10°)` is **not** the parallel at 10° latitude — it bows
  poleward. This matches S2 and `GeometryOps`' spherical model.
- **Rings are implicitly closed**: give each vertex once; do not repeat the
  first. The last edge connects the final vertex back to the first.
- **Input winding order is irrelevant.** A loop splits the sphere in two;
  `from_lon_lat` normalizes so the smaller region (area ≤ a hemisphere) is the
  interior. On **output**, `rings()` follows the S2 convention — interior to the
  left of each directed edge, with holes wound opposite to shells.
- **Area is on the unit sphere**, in **steradians** (range `[0, 4π]`); multiply
  by `R²` for a physical area.

### Tolerance posture

`intersection` snaps output vertices at S2's default *intersection merge radius*
(`S2::kIntersectionMergeRadius`, ≈ `1.8e-15` rad ≈ 11 nm on Earth) — the minimum
that guarantees a topologically valid result while keeping vertices essentially
exact. Intersecting disjoint polygons, or polygons that only touch along an edge
or at a vertex, yields an **empty** result (`is_empty()` is `true`).

## Running in the browser (WebAssembly)

The same s2geometry kernel compiles to **WebAssembly** via Emscripten, with a
small JavaScript/TypeScript wrapper exposing `SphericalPolygon` and `Delaunay`
classes — so spherical area, great-circle intersection, and spherical
Delaunay/Voronoi run client-side in the browser (and in Node).

```js
import { load } from "./wasm/dist/s2bindings.mjs";
const s2 = await load();
const a = s2.SphericalPolygon.fromLonLat([[0, 0], [90, 0], [0, 90]]);
a.area(); // π/2 steradians
```

Build it with `bash wasm/build.sh` (needs the Emscripten SDK on `PATH`). See
[`wasm/README.md`](wasm/README.md) for the full API, the build/verify steps, and
how the C++ → wasm build is wired (it cross-compiles a static OpenSSL `libcrypto`
for s2geometry's `ExactFloat`). The CI `wasm` job builds and smoke-tests it.

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
cargo test      # runs the spherical-geometry + FFI tests
```

The first build compiles abseil + s2geometry from source and takes several
minutes; subsequent builds are incremental.

## Depending on this crate

The crate is distributed via git (it is not published to crates.io). Pin a tag
for reproducibility:

```toml
[dependencies]
s2bindings = { git = "https://github.com/EarthSciML/s2bindings.rs", tag = "v0.1.0" }
```

Downstream builds need the same C++ toolchain (see [Prerequisites](#prerequisites)),
and Cargo fetches the vendored submodules automatically as part of the git
dependency checkout.

## Continuous integration

`.github/workflows/ci.yml` runs on Ubuntu: it checks out submodules
recursively, installs the OpenSSL toolchain via `apt`, then runs
`cargo fmt --check`, `cargo build`, `cargo test`, and `cargo clippy` across the
workspace. CI is the canonical build gate.

## Scope notes

- **s2geography is intentionally not built.** The kernel —
  `intersect_polygon` (`S2Polygon::InitToIntersection`) and spherical area
  (`S2Polygon::GetArea`) — lives in **s2geometry** itself, so s2geography is not
  required. s2geography 0.3.0 additionally pulls in a build-time `nanoarrow`
  fetch and a mandatory installed-abseil lookup, neither of which is needed
  here; it can be added later if WKT/WKB/GeoArrow I/O is wanted.
- **Vendoring choice.** abseil + s2geometry are pinned git submodules rather
  than system packages because s2geometry is not apt-installable and abseil's
  ABI is sensitive to the exact version and C++ standard it is built with.
  Vendoring makes the build hermetic and reproducible across the (RHEL) cluster
  and (Ubuntu) CI.

## License

MIT — see [LICENSE](LICENSE).
