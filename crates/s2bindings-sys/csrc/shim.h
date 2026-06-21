/* C ABI shim over the s2geometry C++ library.
 *
 * Two surfaces are exposed:
 *
 *   1. A trivial identity / smoke surface (s2bindings_s2_max_level,
 *      s2bindings_unit_point_norm) used to prove the vendored C++ stack links
 *      and is callable from Rust.
 *
 *   2. The spherical-geometry kernel: building a polygon from a great-circle
 *      loop, computing its area, and intersecting two polygons
 *      (S2Polygon::InitToIntersection). Polygons are passed across the
 *      boundary as opaque heap-allocated handles; the caller owns each handle
 *      and must release it with s2bindings_polygon_free.
 *
 * Geometry model: all edges are geodesics (great-circle arcs) and all areas
 * are measured on the unit sphere (steradians, range [0, 4*pi]). See the Rust
 * crate docs for the manifold / orientation contract. */
#ifndef S2BINDINGS_SHIM_H
#define S2BINDINGS_SHIM_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- smoke surface ------------------------------------------------------ */

/* Returns S2CellId::kMaxLevel (== 30). Compile/link identity check. */
int s2bindings_s2_max_level(void);

/* Builds S2LatLng::FromDegrees(lat_deg, lng_deg), converts it to an S2Point
 * (a unit vector), and returns that point's Euclidean norm (== 1.0 for any
 * valid input). Proves the linked library executes real geometry math. */
double s2bindings_unit_point_norm(double lat_deg, double lng_deg);

/* ---- spherical-geometry kernel ------------------------------------------ */

/* Opaque handle wrapping a heap-allocated S2Polygon. */
typedef struct s2bindings_polygon s2bindings_polygon;

/* Builds a polygon from a single great-circle loop, given as parallel
 * latitude/longitude arrays in degrees, each of length `n`.
 *
 * The loop is implicitly closed: pass each distinct vertex exactly once and do
 * NOT repeat the first vertex at the end. Edges connect consecutive vertices
 * (and the last back to the first) along great-circle arcs. The loop is
 * normalized so that its enclosed area is at most 2*pi, i.e. the "smaller" of
 * the two regions the loop divides the sphere into is taken as the interior;
 * input vertex order (CW vs CCW) therefore does not matter.
 *
 * On success returns a non-null handle and does not touch `err_buf`.
 * On invalid / degenerate input (fewer than 3 vertices, duplicate or antipodal
 * vertices, self-intersection, non-finite coordinates) returns NULL; if
 * `err_buf` is non-null and `err_buf_len > 0`, a NUL-terminated human-readable
 * reason is written (truncated to fit). */
s2bindings_polygon *s2bindings_polygon_new(const double *lat_deg,
                                           const double *lng_deg, size_t n,
                                           char *err_buf, size_t err_buf_len);

/* Releases a handle returned by s2bindings_polygon_new or
 * s2bindings_polygon_intersection. NULL is accepted and ignored. */
void s2bindings_polygon_free(s2bindings_polygon *p);

/* Area enclosed by the polygon, in steradians (unit sphere): range [0, 4*pi].
 * An empty polygon has area 0. Multiply by R*R for a physical area on a sphere
 * of radius R. */
double s2bindings_polygon_area(const s2bindings_polygon *p);

/* Returns 1 if the polygon encloses no area (e.g. the result of intersecting
 * two disjoint or edge-tangent polygons), 0 otherwise. */
int s2bindings_polygon_is_empty(const s2bindings_polygon *p);

/* Number of loops in the polygon. A simple polygon has 1; an empty polygon has
 * 0; results with holes or multiple disjoint pieces have more. */
int s2bindings_polygon_num_loops(const s2bindings_polygon *p);

/* Number of vertices in loop `loop_index` (in [0, num_loops)), or -1 if the
 * index is out of range. The loop is not closed: the returned count does not
 * include a repeated final vertex. */
int s2bindings_polygon_loop_num_vertices(const s2bindings_polygon *p,
                                         int loop_index);

/* Returns 1 if loop `loop_index` bounds a hole (its interior lies outside the
 * loop), 0 if it is a shell, or -1 if the index is out of range. */
int s2bindings_polygon_loop_is_hole(const s2bindings_polygon *p,
                                    int loop_index);

/* Copies loop `loop_index`'s vertices into the caller-provided arrays as
 * latitude/longitude in degrees. Both arrays must have capacity of at least
 * s2bindings_polygon_loop_num_vertices(p, loop_index) elements. Does nothing if
 * the index is out of range. Vertices are in S2 order (interior on the left);
 * the loop is not closed. */
void s2bindings_polygon_loop_vertices(const s2bindings_polygon *p,
                                      int loop_index, double *lat_deg_out,
                                      double *lng_deg_out);

/* Computes the intersection (great-circle clip) of polygons `a` and `b`,
 * snapped at S2's default intersection merge radius (~1.8e-15 rad), and returns
 * it as a new handle the caller owns. The result may be empty (disjoint or
 * edge-tangent inputs). Returns NULL only on internal failure; if `err_buf` is
 * non-null and `err_buf_len > 0`, a NUL-terminated reason is written. */
s2bindings_polygon *s2bindings_polygon_intersection(const s2bindings_polygon *a,
                                                    const s2bindings_polygon *b,
                                                    char *err_buf,
                                                    size_t err_buf_len);

/* ---- spherical Delaunay / Voronoi connectivity --------------------------- */

/* Opaque handle wrapping a spherical Delaunay triangulation of a set of
 * generator points, together with the dual Voronoi vertices (the per-triangle
 * circumcenters). The triangulation is the set of faces of the 3-D convex hull
 * of the generator points taken as unit vectors -- which, for points on a
 * sphere, is exactly their Delaunay triangulation. */
typedef struct s2bindings_delaunay s2bindings_delaunay;

/* Builds the spherical Delaunay triangulation of `n` generator points, given as
 * parallel latitude/longitude arrays in degrees.
 *
 * DETERMINISM CONTRACT (normative -- any binding reproducing this output must
 * follow it):
 *   - Generator cells are identified by their input index, 0..n-1.
 *   - Triangles are the faces of the 3-D convex hull of the generators as unit
 *     vectors. Every orientation decision is made with s2geometry's robust
 *     orientation predicate (a double-precision result guarded by a Shewchuk
 *     static error filter, falling back to ExactFloat arbitrary precision), so
 *     the triangulation is a deterministic function of the input coordinates --
 *     independent of insertion order and free of floating-point tie ambiguity.
 *   - Each triangle is emitted CCW as seen from outside the sphere, rotated so
 *     its smallest cell index comes first; the triangle list is sorted
 *     lexicographically by (i, j, k). The Voronoi vertex with index v is the
 *     circumcenter of triangle v.
 *   - The contract resolves every input whose generators are not EXACTLY
 *     cospherically degenerate (four exactly-coplanar points). Coordinates
 *     produced from lon/lat via trigonometry are never exactly coplanar; an
 *     exactly-degenerate input is reported as an error rather than resolved by
 *     an ad-hoc rule.
 *
 * Requires n >= 4 generators that are not all coplanar (a 3-D hull needs a
 * non-degenerate tetrahedron). On success returns a non-null handle and does
 * not touch `err_buf`. On invalid input (n < 4, non-finite coordinates,
 * duplicate generators, all generators coplanar, or an exactly-degenerate
 * configuration) returns NULL; if `err_buf` is non-null and `err_buf_len > 0` a
 * NUL-terminated human-readable reason is written (truncated to fit). */
s2bindings_delaunay *s2bindings_delaunay_new(const double *lat_deg,
                                             const double *lng_deg, size_t n,
                                             char *err_buf, size_t err_buf_len);

/* Releases a handle returned by s2bindings_delaunay_new. NULL is accepted and
 * ignored. */
void s2bindings_delaunay_free(s2bindings_delaunay *d);

/* Number of generator cells (== the `n` passed to s2bindings_delaunay_new), or
 * 0 for a null handle. */
int s2bindings_delaunay_num_points(const s2bindings_delaunay *d);

/* Number of Delaunay triangles == number of dual Voronoi vertices. For n
 * generators this is exactly 2*n - 4 (Euler's formula for a triangulated
 * sphere). Returns 0 for a null handle. */
int s2bindings_delaunay_num_triangles(const s2bindings_delaunay *d);

/* Copies the triangles into `out_ijk` as 3 cell indices per triangle (a flat
 * array of length 3 * num_triangles): out_ijk[3*t+0..2] are the cell indices of
 * triangle `t`, CCW as seen from outside the sphere and rotated so the smallest
 * index is first. The caller-provided buffer must have capacity at least
 * 3 * s2bindings_delaunay_num_triangles(d). Does nothing for a null handle. */
void s2bindings_delaunay_triangles(const s2bindings_delaunay *d, int *out_ijk);

/* Copies the dual Voronoi vertices (the per-triangle circumcenters, i.e. the
 * unit points equidistant from each triangle's three generators) into the
 * caller-provided arrays as latitude/longitude in degrees. Both arrays must
 * have capacity at least s2bindings_delaunay_num_triangles(d). The circumcenter
 * of triangle `t` is written at index `t`. Does nothing for a null handle. */
void s2bindings_delaunay_circumcenters(const s2bindings_delaunay *d,
                                       double *lat_deg_out, double *lng_deg_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S2BINDINGS_SHIM_H */
