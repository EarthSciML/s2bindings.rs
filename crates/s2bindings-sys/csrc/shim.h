/* C ABI shim over the s2geometry C++ library.
 *
 * Scaffold stage: only a trivial identity / smoke surface is exposed, used to
 * prove the vendored C++ stack links and is callable from Rust. The spherical
 * geometry kernel (intersect_polygon, area) is added in a follow-up. */
#ifndef S2BINDINGS_SHIM_H
#define S2BINDINGS_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns S2CellId::kMaxLevel (== 30). Compile/link identity check. */
int s2bindings_s2_max_level(void);

/* Builds S2LatLng::FromDegrees(lat_deg, lng_deg), converts it to an S2Point
 * (a unit vector), and returns that point's Euclidean norm (== 1.0 for any
 * valid input). Proves the linked library executes real geometry math. */
double s2bindings_unit_point_norm(double lat_deg, double lng_deg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S2BINDINGS_SHIM_H */
