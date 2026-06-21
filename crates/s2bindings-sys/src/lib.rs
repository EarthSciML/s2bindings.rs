//! Low-level FFI bindings (`-sys` crate) over the vendored s2geometry C++ stack.
//!
//! This crate links Google's [s2geometry] spherical-geometry engine (built from
//! vendored `abseil-cpp` + `s2geometry` submodules via a CMake superbuild) into
//! Rust and exposes the raw C ABI of the shim in `csrc/`.
//!
//! Two surfaces are declared here:
//!
//! * a trivial identity / smoke surface ([`s2_max_level`], [`unit_point_norm`])
//!   that proves the C++ stack compiles, links, and is callable; and
//! * the raw spherical-geometry kernel ([`s2bindings_polygon_new`],
//!   [`s2bindings_polygon_intersection`], [`s2bindings_polygon_area`], and the
//!   loop accessors) over [`s2bindings_polygon`] handles.
//!
//! As a `-sys` crate the kernel functions are exposed exactly as the C ABI
//! presents them: `unsafe`, pointer-based, and without lifetime tracking. The
//! safe, idiomatic Rust API (a `SphericalPolygon` newtype with `(lon, lat)`
//! conversion, RAII, and `Result`-based error handling) lives in the companion
//! `s2bindings` crate, which is what downstream code should depend on.
//!
//! [s2geometry]: https://github.com/google/s2geometry

use std::os::raw::{c_char, c_int};

extern "C" {
    /// Returns `S2CellId::kMaxLevel` (== 30) from the linked C++ library.
    fn s2bindings_s2_max_level() -> c_int;

    /// Returns the Euclidean norm of `S2LatLng::FromDegrees(lat, lng).ToPoint()`.
    fn s2bindings_unit_point_norm(lat_deg: f64, lng_deg: f64) -> f64;
}

/// Maximum S2 cell level (always 30), read from the linked s2geometry library.
///
/// This is the simplest possible identity check: it proves the s2geometry
/// headers compiled and the static library linked, without exercising any
/// runtime geometry.
pub fn s2_max_level() -> i32 {
    // SAFETY: the C function takes no arguments and returns a plain `int`.
    unsafe { s2bindings_s2_max_level() }
}

/// Euclidean norm of the 3-D unit vector that s2geometry derives from a
/// `(latitude, longitude)` pair given in degrees.
///
/// For any valid latitude/longitude the result is `1.0` (within floating-point
/// tolerance): `S2LatLng::ToPoint` returns a point on the unit sphere. This
/// exercises real s2geometry math at run time, confirming the linked library
/// actually executes rather than merely linking.
pub fn unit_point_norm(lat_deg: f64, lng_deg: f64) -> f64 {
    // SAFETY: the C function takes two `double`s and returns a `double`; no
    // pointers or lifetimes are involved.
    unsafe { s2bindings_unit_point_norm(lat_deg, lng_deg) }
}

/// Opaque handle to a heap-allocated `S2Polygon`, owned by the C++ side.
///
/// Values are produced by [`s2bindings_polygon_new`] /
/// [`s2bindings_polygon_intersection`] and must be released with
/// [`s2bindings_polygon_free`]. The zero-sized, non-constructible body follows
/// the standard opaque-FFI-type idiom: it can only be referred to behind a
/// pointer, never instantiated or dereferenced from Rust.
#[repr(C)]
pub struct s2bindings_polygon {
    _data: [u8; 0],
    // Mark as `!Send`/`!Sync` and non-constructible outside this crate.
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}

extern "C" {
    /// Builds a polygon from a single great-circle loop given as parallel
    /// latitude/longitude arrays (degrees) of length `n`. Returns null on
    /// invalid/degenerate input, writing a NUL-terminated reason into
    /// `err_buf` (capacity `err_buf_len`) when that buffer is non-null.
    ///
    /// The loop must not repeat its first vertex; it is normalized so the
    /// enclosed area is at most a hemisphere.
    pub fn s2bindings_polygon_new(
        lat_deg: *const f64,
        lng_deg: *const f64,
        n: usize,
        err_buf: *mut c_char,
        err_buf_len: usize,
    ) -> *mut s2bindings_polygon;

    /// Releases a handle (null is accepted and ignored).
    pub fn s2bindings_polygon_free(p: *mut s2bindings_polygon);

    /// Area enclosed by the polygon in steradians (unit sphere), range
    /// `[0, 4*pi]`; `0.0` for an empty polygon or null handle.
    pub fn s2bindings_polygon_area(p: *const s2bindings_polygon) -> f64;

    /// Returns `1` if the polygon encloses no area, `0` otherwise.
    pub fn s2bindings_polygon_is_empty(p: *const s2bindings_polygon) -> c_int;

    /// Number of loops (0 for empty, 1 for a simple polygon, more with holes
    /// or disjoint pieces).
    pub fn s2bindings_polygon_num_loops(p: *const s2bindings_polygon) -> c_int;

    /// Vertex count of loop `loop_index`, or `-1` if the index is out of range.
    pub fn s2bindings_polygon_loop_num_vertices(
        p: *const s2bindings_polygon,
        loop_index: c_int,
    ) -> c_int;

    /// `1` if loop `loop_index` bounds a hole, `0` if a shell, `-1` if the
    /// index is out of range.
    pub fn s2bindings_polygon_loop_is_hole(
        p: *const s2bindings_polygon,
        loop_index: c_int,
    ) -> c_int;

    /// Copies loop `loop_index`'s vertices into the caller's `lat_deg_out` /
    /// `lng_deg_out` arrays (degrees), each of capacity at least
    /// [`s2bindings_polygon_loop_num_vertices`]. The loop is not closed.
    pub fn s2bindings_polygon_loop_vertices(
        p: *const s2bindings_polygon,
        loop_index: c_int,
        lat_deg_out: *mut f64,
        lng_deg_out: *mut f64,
    );

    /// Intersection (great-circle clip) of `a` and `b`, snapped at S2's default
    /// intersection merge radius. Returns a new owned handle (possibly empty),
    /// or null on internal failure (writing a reason into `err_buf`).
    pub fn s2bindings_polygon_intersection(
        a: *const s2bindings_polygon,
        b: *const s2bindings_polygon,
        err_buf: *mut c_char,
        err_buf_len: usize,
    ) -> *mut s2bindings_polygon;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn max_level_is_30() {
        assert_eq!(
            s2_max_level(),
            30,
            "linked s2geometry should report S2CellId::kMaxLevel == 30"
        );
    }

    #[test]
    fn unit_point_has_unit_norm() {
        // A spread of lat/lng pairs, including near-pole and antimeridian, must
        // all map onto the unit sphere.
        for (lat, lng) in [
            (0.0, 0.0),
            (45.0, -120.0),
            (-33.5, 151.2),
            (89.9, 179.9),
            (-89.9, -179.9),
        ] {
            let n = unit_point_norm(lat, lng);
            assert!(
                (n - 1.0).abs() < 1e-12,
                "unit-vector norm for ({lat}, {lng}) was {n}, expected ~1.0"
            );
        }
    }

    #[test]
    fn raw_polygon_roundtrip_area_and_free() {
        // A spherical octant — vertices at (0,0), (0,90), (90,0) in lat/lng —
        // encloses exactly 1/8 of the sphere: 4*pi / 8 = pi/2 steradians.
        let lat = [0.0_f64, 0.0, 90.0];
        let lng = [0.0_f64, 90.0, 0.0];
        // SAFETY: arrays are length 3 and outlive the call; no error buffer.
        let p = unsafe {
            s2bindings_polygon_new(lat.as_ptr(), lng.as_ptr(), 3, std::ptr::null_mut(), 0)
        };
        assert!(!p.is_null(), "octant should be a valid loop");
        // SAFETY: `p` is a valid handle.
        let area = unsafe { s2bindings_polygon_area(p) };
        assert!(
            (area - std::f64::consts::FRAC_PI_2).abs() < 1e-9,
            "octant area was {area}, expected pi/2"
        );
        // SAFETY: `p` is a valid handle; release it.
        unsafe { s2bindings_polygon_free(p) };
    }
}
