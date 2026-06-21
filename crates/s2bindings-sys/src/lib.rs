//! Low-level FFI bindings (`-sys` crate) over the vendored s2geometry C++ stack.
//!
//! This crate links Google's [s2geometry] spherical-geometry engine (built from
//! vendored `abseil-cpp` + `s2geometry` submodules via a CMake superbuild) into
//! Rust. At this **scaffold** stage it deliberately exposes only a trivial
//! identity / smoke surface, whose sole purpose is to prove that the C++ stack
//! compiles, links, and is callable from Rust.
//!
//! The spherical geometry kernel itself -- `intersect_polygon`
//! (`S2Polygon::InitToIntersection`) and spherical area (`S2Polygon::GetArea`)
//! plus safe wrappers -- lands in a follow-up that builds on this scaffold.
//!
//! [s2geometry]: https://github.com/google/s2geometry

use std::os::raw::c_int;

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
}
