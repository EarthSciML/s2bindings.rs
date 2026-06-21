//! Safe Rust API for **spherical** polygon operations — intersection (clipping)
//! and area — backed by Google's [s2geometry] C++ engine via the
//! [`s2bindings-sys`] crate.
//!
//! Rust's planar geometry crates treat coordinates as points in the Cartesian
//! plane, which is wrong for lon/lat data spanning large areas or near the
//! poles / antimeridian. This crate instead operates on the sphere, matching
//! the model used by Python's [spherely] and R's [s2].
//!
//! # Geometry model (the manifold / edge contract)
//!
//! * **Coordinates** are `(longitude, latitude)` pairs in **degrees**
//!   (`x = lon`, `y = lat`), the order used by GeoJSON and `GeometryOps`.
//! * **Edges are geodesics** — great-circle arcs between consecutive vertices.
//!   This is the key difference from planar clipping: the edge between
//!   `(0°, 10°)` and `(20°, 10°)` is *not* the parallel at 10° latitude; it
//!   bows poleward. Construct rings with this in mind.
//! * **Rings are implicitly closed**: provide each vertex once and do not
//!   repeat the first vertex. The final edge connects the last vertex back to
//!   the first.
//! * **Winding order is irrelevant on input.** A loop divides the sphere into
//!   two regions; [`SphericalPolygon::from_lon_lat`] normalizes so the smaller
//!   region (area ≤ a hemisphere) is the interior. On *output* (see
//!   [`SphericalPolygon::rings`]) loops follow the S2 convention: the interior
//!   lies to the left of each directed edge, and holes wind oppositely to
//!   shells.
//! * **Area is on the unit sphere**, measured in **steradians** (range
//!   `[0, 4π]`). Multiply by `R²` (or use [`SphericalPolygon::area_on_sphere`])
//!   for a physical area on a sphere of radius `R`.
//!
//! # Tolerance posture
//!
//! [`SphericalPolygon::intersection`] snaps output vertices at S2's default
//! intersection merge radius (≈ `1.8e-15` radians ≈ 11 nm on Earth) — the
//! minimum that guarantees a topologically valid result. Vertices are therefore
//! exact for practical purposes; intersecting disjoint or merely edge-touching
//! polygons yields an [empty](SphericalPolygon::is_empty) result.
//!
//! # Example
//!
//! ```
//! use s2bindings::SphericalPolygon;
//!
//! // Two spherical "octant" sectors (bounded by the equator and meridians),
//! // each covering a quarter-hemisphere = π/2 steradians.
//! let a = SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (90.0, 0.0), (0.0, 90.0)])?;
//! let b = SphericalPolygon::from_lon_lat(&[(45.0, 0.0), (135.0, 0.0), (45.0, 90.0)])?;
//!
//! let clip = a.intersection(&b)?;
//! assert!(!clip.is_empty());
//! // Overlap is the lon∈[45°,90°] northern sector: π/4 steradians.
//! assert!((clip.area() - std::f64::consts::FRAC_PI_4).abs() < 1e-9);
//! # Ok::<(), s2bindings::S2Error>(())
//! ```
//!
//! [s2geometry]: https://github.com/google/s2geometry
//! [spherely]: https://github.com/benbovy/spherely
//! [s2]: https://github.com/r-spatial/s2

use std::fmt;
use std::os::raw::{c_char, c_int};

use s2bindings_sys as sys;

/// Size of the stack buffer passed to the C shim for error messages.
const ERR_BUF_LEN: usize = 256;

/// An error from constructing a polygon or performing a spherical operation.
///
/// Carries the human-readable reason reported by the underlying s2geometry
/// validation (e.g. "Edge X crosses edge Y", "at least 3 vertices").
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct S2Error {
    message: String,
}

impl S2Error {
    fn new(message: impl Into<String>) -> Self {
        S2Error {
            message: message.into(),
        }
    }

    /// The underlying failure reason.
    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for S2Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "s2 geometry error: {}", self.message)
    }
}

impl std::error::Error for S2Error {}

/// A single boundary loop of a [`SphericalPolygon`].
#[derive(Debug, Clone, PartialEq)]
pub struct Ring {
    /// `true` if this loop bounds a hole (its interior lies *outside* the loop);
    /// `false` for an outer shell. Holes wind opposite to shells.
    pub is_hole: bool,
    /// Loop vertices as `(longitude, latitude)` in degrees, following the S2
    /// orientation convention (interior on the left). The ring is **not**
    /// closed: the first vertex is not repeated at the end.
    pub vertices: Vec<(f64, f64)>,
}

/// A polygon on the unit sphere with great-circle (geodesic) edges.
///
/// Owns a handle to an `S2Polygon` on the C++ side and frees it on drop. See
/// the [crate documentation](crate) for the full geometry / edge contract.
///
/// Not `Send`/`Sync`: the wrapped `S2Polygon` builds an internal spatial index
/// lazily and is not safe to share across threads without synchronization.
pub struct SphericalPolygon {
    handle: *mut sys::s2bindings_polygon,
}

impl SphericalPolygon {
    /// Builds a polygon from a single great-circle loop of `(longitude,
    /// latitude)` vertices in degrees.
    ///
    /// The loop is implicitly closed; do not repeat the first vertex. Input
    /// winding order does not matter (see the [crate](crate) docs).
    ///
    /// # Errors
    ///
    /// Returns [`S2Error`] if the loop is invalid or degenerate: fewer than 3
    /// vertices, duplicate or antipodal adjacent vertices, a self-intersection,
    /// or non-finite coordinates.
    pub fn from_lon_lat(vertices: &[(f64, f64)]) -> Result<Self, S2Error> {
        if vertices.len() < 3 {
            return Err(S2Error::new(
                "polygon loop needs at least 3 distinct vertices",
            ));
        }
        // The shim takes parallel latitude / longitude arrays (the S2LatLng
        // constructor order), so split the (lon, lat) input.
        let lats: Vec<f64> = vertices.iter().map(|&(_lon, lat)| lat).collect();
        let lngs: Vec<f64> = vertices.iter().map(|&(lon, _lat)| lon).collect();

        let mut err = [0 as c_char; ERR_BUF_LEN];
        // SAFETY: `lats` and `lngs` each hold exactly `vertices.len()` finite
        // slots and outlive the call; `err` is a valid writable buffer of
        // `ERR_BUF_LEN` bytes.
        let handle = unsafe {
            sys::s2bindings_polygon_new(
                lats.as_ptr(),
                lngs.as_ptr(),
                vertices.len(),
                err.as_mut_ptr(),
                ERR_BUF_LEN,
            )
        };
        if handle.is_null() {
            return Err(S2Error::new(err_buf_to_string(&err)));
        }
        Ok(SphericalPolygon { handle })
    }

    /// Area enclosed by the polygon, in **steradians** (unit sphere), in the
    /// range `[0, 4π]`. An [empty](Self::is_empty) polygon has area `0`.
    pub fn area(&self) -> f64 {
        // SAFETY: `self.handle` is a valid, non-null handle for `&self`'s life.
        unsafe { sys::s2bindings_polygon_area(self.handle) }
    }

    /// Physical area on a sphere of the given `radius`, i.e. [`area`](Self::area)
    /// scaled by `radius²`. Pass Earth's mean radius (≈ 6_371_008.8 m) to get
    /// square metres, for example.
    pub fn area_on_sphere(&self, radius: f64) -> f64 {
        self.area() * radius * radius
    }

    /// Returns `true` if the polygon encloses no area — for instance the result
    /// of intersecting two disjoint or merely edge-touching polygons.
    pub fn is_empty(&self) -> bool {
        // SAFETY: valid handle.
        unsafe { sys::s2bindings_polygon_is_empty(self.handle) != 0 }
    }

    /// Number of boundary loops: `0` when empty, `1` for a simple polygon, more
    /// when the polygon has holes or several disjoint pieces.
    pub fn num_loops(&self) -> usize {
        // SAFETY: valid handle; the count is non-negative.
        let n = unsafe { sys::s2bindings_polygon_num_loops(self.handle) };
        n.max(0) as usize
    }

    /// Intersection (great-circle clip) of `self` and `other`.
    ///
    /// The result may be [empty](Self::is_empty) when the inputs are disjoint or
    /// only touch along an edge or vertex. See the [crate](crate) docs for the
    /// tolerance posture.
    ///
    /// # Errors
    ///
    /// Returns [`S2Error`] only on an internal s2geometry failure (which should
    /// not occur for polygons built via [`from_lon_lat`](Self::from_lon_lat)).
    pub fn intersection(&self, other: &SphericalPolygon) -> Result<SphericalPolygon, S2Error> {
        let mut err = [0 as c_char; ERR_BUF_LEN];
        // SAFETY: both handles are valid and non-null; `err` is a valid buffer.
        let handle = unsafe {
            sys::s2bindings_polygon_intersection(
                self.handle,
                other.handle,
                err.as_mut_ptr(),
                ERR_BUF_LEN,
            )
        };
        if handle.is_null() {
            return Err(S2Error::new(err_buf_to_string(&err)));
        }
        Ok(SphericalPolygon { handle })
    }

    /// Returns the polygon's boundary loops as [`Ring`]s, with vertices in
    /// `(longitude, latitude)` degrees. Empty for an empty polygon.
    pub fn rings(&self) -> Vec<Ring> {
        let num_loops = self.num_loops();
        let mut rings = Vec::with_capacity(num_loops);
        for i in 0..num_loops {
            let idx = i as c_int;
            // SAFETY: `idx` is in `[0, num_loops)`; valid handle.
            let nv = unsafe { sys::s2bindings_polygon_loop_num_vertices(self.handle, idx) };
            if nv <= 0 {
                continue;
            }
            let nv = nv as usize;
            // SAFETY: valid handle and in-range index.
            let is_hole = unsafe { sys::s2bindings_polygon_loop_is_hole(self.handle, idx) } == 1;

            let mut lat = vec![0.0_f64; nv];
            let mut lng = vec![0.0_f64; nv];
            // SAFETY: both buffers have capacity `nv == loop_num_vertices(idx)`,
            // which is exactly what the shim writes; valid handle and index.
            unsafe {
                sys::s2bindings_polygon_loop_vertices(
                    self.handle,
                    idx,
                    lat.as_mut_ptr(),
                    lng.as_mut_ptr(),
                );
            }
            // Re-pair as (lon, lat) for the public convention.
            let vertices = lng.into_iter().zip(lat).collect();
            rings.push(Ring { is_hole, vertices });
        }
        rings
    }
}

impl Drop for SphericalPolygon {
    fn drop(&mut self) {
        // SAFETY: `self.handle` came from the shim and has not been freed; the
        // shim accepts (and ignores) null defensively in any case.
        unsafe { sys::s2bindings_polygon_free(self.handle) };
    }
}

impl fmt::Debug for SphericalPolygon {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SphericalPolygon")
            .field("num_loops", &self.num_loops())
            .field("area_steradians", &self.area())
            .finish()
    }
}

/// Reads a NUL-terminated C error message out of a `c_char` buffer.
fn err_buf_to_string(buf: &[c_char]) -> String {
    let bytes: Vec<u8> = buf
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c as u8)
        .collect();
    if bytes.is_empty() {
        "unknown s2 geometry error".to_string()
    } else {
        String::from_utf8_lossy(&bytes).into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f64::consts::{FRAC_PI_2, FRAC_PI_4, PI};

    // A spherical "octant" sector spanning longitudes [lon0, lon1] in the
    // northern hemisphere, bounded below by the equator and on the sides by
    // meridians (all great circles), converging at the north pole. Its area is
    // exactly the dihedral angle (lon1 - lon0) in radians.
    fn northern_sector(lon0: f64, lon1: f64) -> SphericalPolygon {
        SphericalPolygon::from_lon_lat(&[(lon0, 0.0), (lon1, 0.0), (lon0, 90.0)])
            .expect("sector is a valid loop")
    }

    // An axis-aligned lon/lat "box". NOTE: only the equatorial edge is a
    // parallel; the top edge is a great-circle arc that bows poleward.
    fn lonlat_box(lon0: f64, lat0: f64, lon1: f64, lat1: f64) -> SphericalPolygon {
        SphericalPolygon::from_lon_lat(&[(lon0, lat0), (lon1, lat0), (lon1, lat1), (lon0, lat1)])
            .expect("box is a valid loop")
    }

    #[test]
    fn octant_area_is_known_reference_value() {
        // One octant of the sphere = 4π/8 = π/2 steradians (Girard: three right
        // angles give excess 3·π/2 − π = π/2).
        let octant =
            SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (90.0, 0.0), (0.0, 90.0)]).unwrap();
        assert!(
            (octant.area() - FRAC_PI_2).abs() < 1e-9,
            "octant area {} should be π/2",
            octant.area()
        );
        assert!(!octant.is_empty());
        assert_eq!(octant.num_loops(), 1);
    }

    #[test]
    fn hemisphere_area_via_equatorial_loop() {
        // Three equally spaced points on the equator bound a hemisphere: 2π sr.
        let hemi =
            SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (120.0, 0.0), (240.0, 0.0)]).unwrap();
        assert!(
            (hemi.area() - 2.0 * PI).abs() < 1e-9,
            "hemisphere area {} should be 2π",
            hemi.area()
        );
    }

    #[test]
    fn input_winding_order_does_not_change_area() {
        let ccw = SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (90.0, 0.0), (0.0, 90.0)]).unwrap();
        let cw = SphericalPolygon::from_lon_lat(&[(0.0, 90.0), (90.0, 0.0), (0.0, 0.0)]).unwrap();
        assert!((ccw.area() - cw.area()).abs() < 1e-12);
        assert!((ccw.area() - FRAC_PI_2).abs() < 1e-9);
    }

    #[test]
    fn area_on_sphere_scales_by_radius_squared() {
        let octant = northern_sector(0.0, 90.0);
        let r = 6_371_008.8_f64; // Earth mean radius, metres.
        assert!((octant.area_on_sphere(r) - octant.area() * r * r).abs() <= f64::EPSILON);
        assert!((octant.area_on_sphere(r) - FRAC_PI_2 * r * r).abs() < 1.0);
    }

    #[test]
    fn intersection_of_overlapping_sectors_is_exact() {
        // [0°,90°] ∩ [45°,135°] = [45°,90°] northern sector → π/4 sr, exactly,
        // because every bounding edge is a shared great circle.
        let a = northern_sector(0.0, 90.0);
        let b = northern_sector(45.0, 135.0);
        let clip = a.intersection(&b).unwrap();

        assert!(!clip.is_empty());
        assert_eq!(clip.num_loops(), 1);
        assert!(
            (clip.area() - FRAC_PI_4).abs() < 1e-9,
            "overlap area {} should be π/4",
            clip.area()
        );
        // Result is smaller than either input.
        assert!(clip.area() < a.area() && clip.area() < b.area());
    }

    #[test]
    fn intersection_is_commutative() {
        let a = northern_sector(0.0, 90.0);
        let b = northern_sector(45.0, 135.0);
        let ab = a.intersection(&b).unwrap();
        let ba = b.intersection(&a).unwrap();
        assert!((ab.area() - ba.area()).abs() < 1e-12);
    }

    #[test]
    fn intersection_with_container_returns_the_contained_polygon() {
        // A ⊂ B  ⇒  A ∩ B = A, exactly, regardless of great-circle bowing.
        let small = lonlat_box(10.0, 10.0, 11.0, 11.0);
        let big = lonlat_box(0.0, 0.0, 40.0, 40.0);
        let clip = small.intersection(&big).unwrap();
        assert_eq!(clip.num_loops(), 1);
        assert!(
            (clip.area() - small.area()).abs() < 1e-12,
            "A∩B area {} should equal A's area {}",
            clip.area(),
            small.area()
        );
    }

    #[test]
    fn intersection_of_disjoint_polygons_is_empty() {
        // Northern sector vs a southern-hemisphere triangle far away: no shared
        // interior, no shared boundary.
        let north = northern_sector(0.0, 90.0);
        let south =
            SphericalPolygon::from_lon_lat(&[(180.0, 0.0), (270.0, 0.0), (180.0, -90.0)]).unwrap();
        let clip = north.intersection(&south).unwrap();
        assert!(clip.is_empty(), "disjoint intersection should be empty");
        assert_eq!(clip.num_loops(), 0);
        assert_eq!(clip.area(), 0.0);
        assert!(clip.rings().is_empty());
    }

    #[test]
    fn intersection_of_edge_tangent_polygons_is_empty() {
        // [0°,90°] and [90°,180°] sectors share only the meridian at 90°.
        // A shared boundary encloses no interior → empty result.
        let a = northern_sector(0.0, 90.0);
        let b = northern_sector(90.0, 180.0);
        let clip = a.intersection(&b).unwrap();
        assert!(clip.is_empty(), "edge-tangent intersection should be empty");
        assert_eq!(clip.area(), 0.0);
    }

    #[test]
    fn rings_round_trip_vertices_in_lon_lat() {
        let octant =
            SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (90.0, 0.0), (0.0, 90.0)]).unwrap();
        let rings = octant.rings();
        assert_eq!(rings.len(), 1);
        let ring = &rings[0];
        assert!(!ring.is_hole);
        assert_eq!(ring.vertices.len(), 3);

        // Every output vertex must be one of the three octant corners (order /
        // winding may be normalized). The north pole's longitude is arbitrary,
        // so match it by latitude alone.
        for &(lon, lat) in &ring.vertices {
            let is_corner = (lat.abs() < 1e-9
                && ((lon - 0.0).abs() < 1e-9 || (lon - 90.0).abs() < 1e-9))
                || (lat - 90.0).abs() < 1e-9;
            assert!(is_corner, "unexpected vertex ({lon}, {lat})");
        }
    }

    #[test]
    fn too_few_vertices_is_an_error() {
        let err = SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (10.0, 0.0)]).unwrap_err();
        assert!(!err.message().is_empty());
    }

    #[test]
    fn degenerate_duplicate_vertices_is_an_error() {
        // Repeated vertices make the loop invalid (zero-length edge).
        let result =
            SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (0.0, 0.0), (10.0, 0.0), (10.0, 10.0)]);
        assert!(result.is_err(), "duplicate vertices should be rejected");
    }

    #[test]
    fn non_finite_coordinates_are_an_error() {
        let result = SphericalPolygon::from_lon_lat(&[(0.0, 0.0), (f64::NAN, 0.0), (10.0, 10.0)]);
        assert!(result.is_err(), "NaN coordinates should be rejected");
    }
}
