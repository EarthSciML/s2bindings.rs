// Node smoke test for the s2bindings WebAssembly build.
//
// Mirrors the native Rust/FFI tests against known closed-form values:
//   - a spherical octant has area π/2,
//   - clipping two octants yields a non-empty spherical triangle,
//   - the six axis points triangulate into an octahedron (8 triangles).
//
// Run after `wasm/build.sh`:   node wasm/test/smoke.mjs
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const { load } = await import(path.join(here, "..", "dist", "s2bindings.mjs"));

const s2 = await load();
let passed = 0;
const ok = (label) => {
  passed++;
  console.log(`  ok ${label}`);
};

// --- liveness ---------------------------------------------------------------
assert.equal(s2.maxLevel(), 30, "S2CellId::kMaxLevel should be 30");
ok("maxLevel() == 30");

// --- polygon area -----------------------------------------------------------
// Octant: vertices (lon,lat) = (0,0),(90,0),(0,90); 1/8 of the sphere = π/2 sr.
const octant = s2.SphericalPolygon.fromLonLat([
  [0, 0],
  [90, 0],
  [0, 90],
]);
assert.ok(Math.abs(octant.area() - Math.PI / 2) < 1e-9, `octant area ${octant.area()} != π/2`);
ok("octant area == π/2");
assert.equal(octant.numLoops(), 1);
assert.equal(octant.isEmpty(), false);
ok("octant has one non-empty loop");

// areaOnSphere scales by R².
const r = 6_371_008.8;
assert.ok(Math.abs(octant.areaOnSphere(r) - (Math.PI / 2) * r * r) < 1e3);
ok("areaOnSphere scales by R²");

// rings() round-trips vertices as a flat [lon,lat,…] Float64Array.
const rings = octant.rings();
assert.equal(rings.length, 1);
assert.equal(rings[0].isHole, false);
assert.ok(rings[0].vertices instanceof Float64Array);
assert.equal(rings[0].vertices.length, 6, "3 vertices × (lon,lat)");
ok("rings() returns a 3-vertex shell");

// --- intersection -----------------------------------------------------------
const a = s2.SphericalPolygon.fromLonLat([
  [0, 0],
  [90, 0],
  [0, 90],
]);
const b = s2.SphericalPolygon.fromLonLat([
  [45, 0],
  [135, 0],
  [45, 90],
]);
const clip = a.intersection(b);
assert.equal(clip.isEmpty(), false, "overlapping octants should clip to a non-empty region");
assert.ok(clip.area() > 0 && clip.area() < a.area(), "clip area should be a proper sub-area");
ok("intersection of overlapping octants is non-empty and smaller");

// Disjoint polygons clip to empty.
const c = s2.SphericalPolygon.fromLonLat([
  [-90, 0],
  [-10, 0],
  [-90, 80],
]);
const empty = a.intersection(c);
assert.equal(empty.isEmpty(), true, "disjoint polygons should clip to empty");
ok("intersection of disjoint polygons is empty");

// --- invalid input throws S2Error ------------------------------------------
assert.throws(
  () => s2.SphericalPolygon.fromLonLat([[0, 0], [1, 1]]),
  (e) => e.name === "S2Error",
  "a 2-vertex loop must be rejected",
);
ok("invalid polygon throws S2Error");

// --- spherical Delaunay -----------------------------------------------------
// Six axis points (±x,±y,±z) → regular octahedron: 6 generators, 2·6−4 = 8 tris.
const oct = s2.Delaunay.fromLonLat([
  [0, 0],
  [90, 0],
  [180, 0],
  [-90, 0],
  [0, 90],
  [0, -90],
]);
assert.equal(oct.numPoints(), 6);
assert.equal(oct.numTriangles(), 8, "octahedron has 8 faces");
ok("octahedron → 6 points, 8 triangles");

const tris = oct.triangles();
assert.ok(tris instanceof Int32Array);
assert.equal(tris.length, 24, "8 triangles × 3 indices");
assert.ok(tris.every((i) => i >= 0 && i < 6), "indices in range");
ok("triangles() returns 24 valid indices");

const cc = oct.circumcenters();
assert.ok(cc instanceof Float64Array);
assert.equal(cc.length, 16, "8 circumcenters × (lon,lat)");
ok("circumcenters() returns 8 lon/lat points");

// Too few generators is rejected.
assert.throws(
  () => s2.Delaunay.fromLonLat([[0, 0], [90, 0], [0, 90]]),
  (e) => e.name === "S2Error",
  "fewer than 4 generators must be rejected",
);
ok("delaunay with <4 points throws S2Error");

// --- cleanup ----------------------------------------------------------------
for (const h of [octant, a, b, clip, c, empty, oct]) h.free();
// free() is idempotent.
octant.free();
ok("handles free() cleanly (idempotent)");

console.log(`\n${passed} checks passed ✓`);
