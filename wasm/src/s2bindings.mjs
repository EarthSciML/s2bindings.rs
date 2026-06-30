// s2bindings — browser/WebAssembly API.
//
// A thin, hand-written wrapper over the C ABI exported by the Emscripten module
// (`s2bindings.core.mjs`, generated from csrc/shim.cc). It mirrors the safe Rust
// API of the `s2bindings` crate: spherical polygons (area + great-circle
// intersection) and a spherical Delaunay/Voronoi triangulation.
//
// Coordinates are (longitude, latitude) in degrees — the GeoJSON order used by
// the Rust API. Inputs and outputs use flat Float64Array / Int32Array buffers of
// interleaved [lon, lat, lon, lat, …]; arrays of [lon, lat] pairs are also
// accepted as input for convenience.
//
//   import { load } from "./s2bindings.mjs";
//   const s2 = await load();
//   const a = s2.SphericalPolygon.fromLonLat([[0,0],[90,0],[0,90]]);
//   a.area();                       // → π/2
//   const b = s2.SphericalPolygon.fromLonLat([[45,0],[135,0],[45,90]]);
//   const clip = a.intersection(b); // great-circle clip
//   clip.free(); a.free(); b.free();
//
// Handles wrap heap-allocated C++ objects: call `.free()` when done (or use
// `using` / a try/finally) to release them.

import createS2Bindings from "./s2bindings.core.mjs";

const F64 = 8; // sizeof(double)
const I32 = 4; // sizeof(int32)
const ERR_LEN = 256; // error-message scratch buffer size

/** Error thrown when the C++ kernel rejects input or fails an operation. */
export class S2Error extends Error {
  constructor(message) {
    super(message);
    this.name = "S2Error";
  }
}

// Normalize accepted coordinate inputs to { lon: Float64Array, lat: Float64Array }.
// Accepts either a flat [lon,lat,lon,lat,…] (Array or Float64Array) or an array
// of [lon,lat] pairs.
function splitLonLat(coords) {
  let lon, lat, n;
  if (ArrayBuffer.isView(coords) || (coords.length > 0 && typeof coords[0] === "number")) {
    if (coords.length % 2 !== 0) {
      throw new S2Error("flat coordinate array must have even length (lon,lat pairs)");
    }
    n = coords.length / 2;
    lon = new Float64Array(n);
    lat = new Float64Array(n);
    for (let i = 0; i < n; i++) {
      lon[i] = coords[2 * i];
      lat[i] = coords[2 * i + 1];
    }
  } else {
    n = coords.length;
    lon = new Float64Array(n);
    lat = new Float64Array(n);
    for (let i = 0; i < n; i++) {
      const p = coords[i];
      lon[i] = p[0];
      lat[i] = p[1];
    }
  }
  return { lon, lat, n };
}

// Low-level marshalling bound to a single loaded module instance.
class Heap {
  constructor(Module) {
    this.M = Module;
  }
  // Allocate and fill a Float64 array on the wasm heap; returns the pointer.
  putF64(values) {
    const ptr = this.M._malloc(values.length * F64 || F64);
    this.M.HEAPF64.set(values, ptr / F64);
    return ptr;
  }
  // Allocate an uninitialized Float64 array of length n.
  allocF64(n) {
    return this.M._malloc((n || 1) * F64);
  }
  allocI32(n) {
    return this.M._malloc((n || 1) * I32);
  }
  // Copy n doubles out of the heap into a fresh Float64Array.
  getF64(ptr, n) {
    return this.M.HEAPF64.slice(ptr / F64, ptr / F64 + n);
  }
  getI32(ptr, n) {
    return this.M.HEAP32.slice(ptr / I32, ptr / I32 + n);
  }
  free(...ptrs) {
    for (const p of ptrs) if (p) this.M._free(p);
  }
  // Run `fn(errPtr, ERR_LEN)`; if it returns 0 (null handle), throw an S2Error
  // carrying the C-side message. Returns the non-null handle otherwise.
  callChecked(fn) {
    const errPtr = this.M._malloc(ERR_LEN);
    this.M.HEAPU8[errPtr] = 0;
    try {
      const handle = fn(errPtr, ERR_LEN);
      if (!handle) {
        throw new S2Error(this.M.UTF8ToString(errPtr) || "s2 operation failed");
      }
      return handle;
    } finally {
      this.M._free(errPtr);
    }
  }
}

/** Earth's mean radius in metres (IUGG), for `areaOnSphere` convenience. */
export const EARTH_RADIUS_M = 6_371_008.8;

function makeApi(Module) {
  const heap = new Heap(Module);
  const M = Module;

  // A spherical polygon backed by a C++ S2Polygon handle.
  class SphericalPolygon {
    // Private: wrap an existing handle. Use fromLonLat() / intersection().
    constructor(handle) {
      if (!handle) throw new S2Error("invalid polygon handle");
      this._h = handle;
    }

    // Build a polygon from a single great-circle loop. `coords` is a flat
    // [lon,lat,…] buffer or an array of [lon,lat] pairs, in degrees. The loop is
    // implicitly closed (do not repeat the first vertex); winding order is
    // irrelevant. Throws S2Error on a degenerate/invalid loop.
    static fromLonLat(coords) {
      const { lon, lat, n } = splitLonLat(coords);
      const latPtr = heap.putF64(lat);
      const lngPtr = heap.putF64(lon);
      try {
        const h = heap.callChecked((errPtr, errLen) =>
          M._s2bindings_polygon_new(latPtr, lngPtr, n, errPtr, errLen),
        );
        return new SphericalPolygon(h);
      } finally {
        heap.free(latPtr, lngPtr);
      }
    }

    _assert() {
      if (!this._h) throw new S2Error("polygon has been freed");
    }

    /** Enclosed area in steradians (unit sphere), range [0, 4π]. */
    area() {
      this._assert();
      return M._s2bindings_polygon_area(this._h);
    }

    /** `area()` scaled by radius² — a physical area on a sphere of that radius. */
    areaOnSphere(radius = EARTH_RADIUS_M) {
      return this.area() * radius * radius;
    }

    /** True if the polygon encloses no area. */
    isEmpty() {
      this._assert();
      return M._s2bindings_polygon_is_empty(this._h) !== 0;
    }

    /** Number of loops (0 empty, 1 simple, more with holes / disjoint pieces). */
    numLoops() {
      this._assert();
      return M._s2bindings_polygon_num_loops(this._h);
    }

    // Great-circle intersection (clip) with `other`. Returns a new polygon (own
    // it / free it); may be empty for disjoint or edge-tangent inputs.
    intersection(other) {
      this._assert();
      other._assert();
      const h = heap.callChecked((errPtr, errLen) =>
        M._s2bindings_polygon_intersection(this._h, other._h, errPtr, errLen),
      );
      return new SphericalPolygon(h);
    }

    // Boundary loops. Each ring is { isHole: boolean, vertices: Float64Array }
    // where vertices is a flat [lon,lat,…] buffer (loop not closed; S2 order,
    // interior to the left of each directed edge, holes wound opposite to shells).
    rings() {
      this._assert();
      const out = [];
      const loops = this.numLoops();
      for (let li = 0; li < loops; li++) {
        const nv = M._s2bindings_polygon_loop_num_vertices(this._h, li);
        const isHole = M._s2bindings_polygon_loop_is_hole(this._h, li) === 1;
        const latPtr = heap.allocF64(nv);
        const lngPtr = heap.allocF64(nv);
        try {
          M._s2bindings_polygon_loop_vertices(this._h, li, latPtr, lngPtr);
          const lat = heap.getF64(latPtr, nv);
          const lon = heap.getF64(lngPtr, nv);
          const vertices = new Float64Array(nv * 2);
          for (let i = 0; i < nv; i++) {
            vertices[2 * i] = lon[i];
            vertices[2 * i + 1] = lat[i];
          }
          out.push({ isHole, vertices });
        } finally {
          heap.free(latPtr, lngPtr);
        }
      }
      return out;
    }

    /** Release the underlying C++ polygon. Idempotent. */
    free() {
      if (this._h) {
        M._s2bindings_polygon_free(this._h);
        this._h = 0;
      }
    }

    // Support `using poly = ...` (explicit-resource-management) where available.
    [Symbol.dispose]() {
      this.free();
    }
  }

  // A spherical Delaunay triangulation (faces of the 3-D convex hull of the
  // generators) plus its dual Voronoi vertices (per-triangle circumcenters).
  class Delaunay {
    constructor(handle) {
      if (!handle) throw new S2Error("invalid delaunay handle");
      this._h = handle;
    }

    // Triangulate >= 4 generator points (not all coplanar). `coords` is a flat
    // [lon,lat,…] buffer or an array of [lon,lat] pairs, in degrees. Throws on
    // degenerate input (see the C ABI determinism contract in csrc/shim.h).
    static fromLonLat(coords) {
      const { lon, lat, n } = splitLonLat(coords);
      const latPtr = heap.putF64(lat);
      const lngPtr = heap.putF64(lon);
      try {
        const h = heap.callChecked((errPtr, errLen) =>
          M._s2bindings_delaunay_new(latPtr, lngPtr, n, errPtr, errLen),
        );
        return new Delaunay(h);
      } finally {
        heap.free(latPtr, lngPtr);
      }
    }

    _assert() {
      if (!this._h) throw new S2Error("delaunay has been freed");
    }

    /** Number of generator points. */
    numPoints() {
      this._assert();
      return M._s2bindings_delaunay_num_points(this._h);
    }

    /** Number of triangles == number of dual Voronoi vertices (2·n − 4). */
    numTriangles() {
      this._assert();
      return M._s2bindings_delaunay_num_triangles(this._h);
    }

    // Triangles as an Int32Array of 3 generator indices each (length 3·numTriangles),
    // CCW as seen from outside the sphere, smallest index first, sorted.
    triangles() {
      this._assert();
      const nt = this.numTriangles();
      const ptr = heap.allocI32(nt * 3);
      try {
        M._s2bindings_delaunay_triangles(this._h, ptr);
        return heap.getI32(ptr, nt * 3);
      } finally {
        heap.free(ptr);
      }
    }

    // Dual Voronoi vertices (per-triangle circumcenters) as a flat [lon,lat,…]
    // Float64Array of length 2·numTriangles; circumcenter of triangle t at [2t,2t+1].
    circumcenters() {
      this._assert();
      const nt = this.numTriangles();
      const latPtr = heap.allocF64(nt);
      const lngPtr = heap.allocF64(nt);
      try {
        M._s2bindings_delaunay_circumcenters(this._h, latPtr, lngPtr);
        const lat = heap.getF64(latPtr, nt);
        const lon = heap.getF64(lngPtr, nt);
        const out = new Float64Array(nt * 2);
        for (let t = 0; t < nt; t++) {
          out[2 * t] = lon[t];
          out[2 * t + 1] = lat[t];
        }
        return out;
      } finally {
        heap.free(latPtr, lngPtr);
      }
    }

    /** Release the underlying triangulation. Idempotent. */
    free() {
      if (this._h) {
        M._s2bindings_delaunay_free(this._h);
        this._h = 0;
      }
    }

    [Symbol.dispose]() {
      this.free();
    }
  }

  return {
    SphericalPolygon,
    Delaunay,
    EARTH_RADIUS_M,
    /** Maximum S2 cell level (always 30); a trivial liveness check. */
    maxLevel: () => M._s2bindings_s2_max_level(),
  };
}

/**
 * Load and instantiate the s2bindings WebAssembly module.
 *
 * @param {object} [moduleArg] Optional Emscripten module overrides (e.g.
 *   `{ locateFile }` to control where `s2bindings.core.wasm` is fetched from).
 * @returns {Promise<{SphericalPolygon, Delaunay, EARTH_RADIUS_M, maxLevel}>}
 */
export async function load(moduleArg) {
  const Module = await createS2Bindings(moduleArg);
  return makeApi(Module);
}

export default { load, S2Error, EARTH_RADIUS_M };
