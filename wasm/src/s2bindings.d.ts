// Type declarations for the s2bindings WebAssembly wrapper.
//
// Coordinates are (longitude, latitude) in degrees. Inputs accept either a flat
// interleaved [lon, lat, …] buffer or an array of [lon, lat] pairs; outputs are
// flat typed arrays.

/** Accepted coordinate input: flat [lon,lat,…] or an array of [lon,lat] pairs. */
export type LonLatInput = Float64Array | number[] | Array<[number, number]>;

/** One boundary loop of a polygon. */
export interface Ring {
  /** True if this loop bounds a hole (interior outside the loop). */
  isHole: boolean;
  /** Flat [lon,lat,…] vertices in degrees (loop not closed, S2 order). */
  vertices: Float64Array;
}

/** Error thrown when the C++ kernel rejects input or an operation fails. */
export class S2Error extends Error {}

/** Earth's mean radius in metres (IUGG). */
export const EARTH_RADIUS_M: number;

/** A spherical polygon backed by a C++ S2Polygon handle. */
export class SphericalPolygon {
  private constructor(handle: number);
  /** Build a polygon from a single great-circle loop (implicitly closed). */
  static fromLonLat(coords: LonLatInput): SphericalPolygon;
  /** Enclosed area in steradians (unit sphere), range [0, 4π]. */
  area(): number;
  /** `area()` scaled by radius² (physical area); defaults to Earth's radius. */
  areaOnSphere(radius?: number): number;
  /** True if the polygon encloses no area. */
  isEmpty(): boolean;
  /** Number of loops (0 empty, 1 simple, more with holes / disjoint pieces). */
  numLoops(): number;
  /** Great-circle intersection (clip); returns a new polygon you must free(). */
  intersection(other: SphericalPolygon): SphericalPolygon;
  /** Boundary loops as { isHole, vertices }. */
  rings(): Ring[];
  /** Release the underlying C++ polygon. Idempotent. */
  free(): void;
  [Symbol.dispose](): void;
}

/** A spherical Delaunay triangulation plus dual Voronoi vertices. */
export class Delaunay {
  private constructor(handle: number);
  /** Triangulate >= 4 generators (not all coplanar). */
  static fromLonLat(coords: LonLatInput): Delaunay;
  /** Number of generator points. */
  numPoints(): number;
  /** Number of triangles == dual Voronoi vertices (2·n − 4). */
  numTriangles(): number;
  /** Triangles: Int32Array of 3 generator indices each (length 3·numTriangles). */
  triangles(): Int32Array;
  /** Dual Voronoi vertices: flat [lon,lat,…] (length 2·numTriangles). */
  circumcenters(): Float64Array;
  /** Release the underlying triangulation. Idempotent. */
  free(): void;
  [Symbol.dispose](): void;
}

/** The loaded module API. */
export interface S2Module {
  SphericalPolygon: typeof SphericalPolygon;
  Delaunay: typeof Delaunay;
  EARTH_RADIUS_M: number;
  /** Maximum S2 cell level (always 30); a trivial liveness check. */
  maxLevel(): number;
}

/** Optional Emscripten module overrides (e.g. `locateFile`). */
export interface ModuleArg {
  locateFile?: (path: string, prefix: string) => string;
  [key: string]: unknown;
}

/** Load and instantiate the s2bindings WebAssembly module. */
export function load(moduleArg?: ModuleArg): Promise<S2Module>;

declare const _default: {
  load: typeof load;
  S2Error: typeof S2Error;
  EARTH_RADIUS_M: number;
};
export default _default;
