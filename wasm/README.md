# s2bindings — WebAssembly / browser target

Google's [s2geometry] spherical-geometry engine, compiled to WebAssembly with
[Emscripten] so the same kernel that backs the Rust API runs in the browser (and
in Node). It exposes spherical **polygon area + great-circle intersection** and a
spherical **Delaunay / Voronoi** triangulation through a small JavaScript/TypeScript
wrapper.

```js
import { load } from "./dist/s2bindings.mjs";

const s2 = await load();

// Coordinates are (longitude, latitude) in degrees; rings are implicitly closed.
const a = s2.SphericalPolygon.fromLonLat([[0, 0], [90, 0], [0, 90]]);
a.area();              // 1.5707963…  (π/2 steradians — a spherical octant)
a.areaOnSphere();     // same, scaled by Earth's mean radius² → m²

const b = s2.SphericalPolygon.fromLonLat([[45, 0], [135, 0], [45, 90]]);
const clip = a.intersection(b);   // great-circle clip; may be empty
for (const ring of clip.rings()) {
  // ring.vertices: Float64Array [lon, lat, …]; ring.isHole: boolean
}

// Spherical Delaunay of >= 4 generators (+ dual Voronoi vertices).
const d = s2.Delaunay.fromLonLat([
  [0, 0], [90, 0], [180, 0], [-90, 0], [0, 90], [0, -90],
]);
d.numTriangles();     // 8   (octahedron: 2·n − 4)
d.triangles();        // Int32Array of 3 generator indices per triangle
d.circumcenters();    // Float64Array [lon, lat, …], one per triangle

// Handles wrap C++ objects — release them when done.
a.free(); b.free(); clip.free(); d.free();
```

`load()` returns a small API object; everything else is synchronous. The wrapper
mirrors the safe Rust API and uses the same `(lon, lat)` / steradian / S2-winding
conventions documented in the [top-level README](../README.md).

## API

`load(moduleArg?)` → `Promise<{ SphericalPolygon, Delaunay, EARTH_RADIUS_M, maxLevel }>`.
Pass `{ locateFile }` in `moduleArg` to control where `s2bindings.core.wasm` is
fetched from (e.g. a CDN path).

| `SphericalPolygon` | |
|---|---|
| `static fromLonLat(coords)` | Build from one great-circle loop. `coords` is a flat `[lon,lat,…]` (`Float64Array` or `number[]`) or an array of `[lon,lat]` pairs. Throws `S2Error` on a degenerate loop. |
| `.area()` | Enclosed area in **steradians** (unit sphere), `[0, 4π]`. |
| `.areaOnSphere(radius?)` | `area()` × `radius²` (defaults to Earth's mean radius). |
| `.isEmpty()` / `.numLoops()` | Emptiness / loop count. |
| `.intersection(other)` | Great-circle clip → a new `SphericalPolygon` (free it). |
| `.rings()` | `Array<{ isHole, vertices: Float64Array }>`, flat `[lon,lat,…]`. |
| `.free()` | Release the handle (idempotent; also `Symbol.dispose`). |

| `Delaunay` | |
|---|---|
| `static fromLonLat(coords)` | Triangulate ≥ 4 generators (not all coplanar). Throws `S2Error` on degenerate input. |
| `.numPoints()` / `.numTriangles()` | Generator count / triangle count (`2·n − 4`). |
| `.triangles()` | `Int32Array`, 3 generator indices per triangle, CCW-from-outside, smallest index first, sorted. |
| `.circumcenters()` | `Float64Array` `[lon,lat,…]`, the dual Voronoi vertices (one per triangle). |
| `.free()` | Release the handle (idempotent; also `Symbol.dispose`). |

Invalid input rejects with a thrown `S2Error` carrying the C++-side message.

## Building

Prerequisites: the **Emscripten SDK** on `PATH`, **CMake ≥ 3.16**, a host C
compiler, `curl`, and the vendored C++ submodules.

```bash
# 1. One-time: install + activate Emscripten, then put it on PATH each shell.
git clone https://github.com/emscripten-core/emsdk.git && cd emsdk
./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
cd -

# 2. One-time: check out the vendored s2geometry + abseil sources.
git submodule update --init --recursive

# 3. Build → wasm/dist/{s2bindings.mjs, s2bindings.core.mjs, s2bindings.core.wasm, …}
bash wasm/build.sh

# 4. Verify.
node wasm/test/smoke.mjs                 # Node smoke test
python3 -m http.server -d wasm 8000      # then open http://localhost:8000/example/
```

`build.sh` (1) cross-compiles a static OpenSSL `libcrypto` for wasm (cached under
`wasm/.cache`), (2) configures the CMake superbuild through `emcmake`, (3) builds
the `s2bindings` module, and (4) stages `wasm/dist/`. Build artifacts are
git-ignored; CI rebuilds them on every push (see `.github/workflows/ci.yml`).

## How it works (and why it is the way it is)

The kernel is C++ (s2geometry + abseil), so the browser path is **Emscripten**,
not `wasm-bindgen` — `wasm-bindgen` targets `wasm32-unknown-unknown`, which has no
libc++/libc and cannot host this C++ stack. Emscripten supplies libc++,
exception support, and a CMake toolchain.

- **Same superbuild, one extra branch.** `crates/s2bindings-sys/csrc/CMakeLists.txt`
  gains an `if(EMSCRIPTEN)` branch that builds abseil + s2geometry + the FFI shim
  **statically** into one self-contained wasm module and exports the C ABI from
  `shim.h` (see `csrc/wasm_exports.txt`) plus `malloc`/`free`. The native
  shared-library path is untouched.
- **OpenSSL for wasm.** s2geometry's `ExactFloat` (the exact-arithmetic fallback
  of its robust predicates) is built on OpenSSL's BIGNUM. The browser has no
  system OpenSSL, so `scripts/build-openssl.sh` cross-compiles a static
  `libcrypto` for wasm and feeds it to the s2geometry build. Only BIGNUM is used;
  no TLS or networking. The exact-arithmetic code path is identical for any
  OpenSSL ≥ 1.1.0, so results match the native build bit-for-bit.
- **No threads / no SharedArrayBuffer.** The module is single-threaded, so it
  loads from any static host — no COOP/COEP cross-origin-isolation headers
  needed.
- **JS over the C ABI.** `src/s2bindings.mjs` is a hand-written wrapper that
  marshals coordinate arrays across the wasm heap and presents the
  `SphericalPolygon` / `Delaunay` classes. The generated Emscripten factory ships
  as `s2bindings.core.mjs`; the wrapper is the package entry point
  `s2bindings.mjs`.

## Layout

```
wasm/
  build.sh                 Orchestrates the full wasm build → dist/
  scripts/build-openssl.sh Cross-compiles libcrypto (BIGNUM) for wasm (cached)
  src/s2bindings.mjs       Hand-written JS wrapper (package entry point)
  src/s2bindings.d.ts      TypeScript declarations
  example/index.html       Browser demo (area, intersection, Delaunay/Voronoi)
  test/smoke.mjs           Node smoke test
  test/browser-test.html   Headless-browser smoke test
  package.json             npm metadata (npm run build / npm test)
  dist/                    Build output (git-ignored)
```

[s2geometry]: https://github.com/google/s2geometry
[Emscripten]: https://emscripten.org/
