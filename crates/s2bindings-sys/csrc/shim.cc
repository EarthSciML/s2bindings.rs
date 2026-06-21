// C ABI shim over the s2geometry C++ library. See shim.h for the contract.

#include "shim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2cell_id.h"
#include "s2/s2edge_crossings.h"
#include "s2/s2error.h"
#include "s2/s2latlng.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"
#include "s2/s2predicates.h"
#include "s2/util/math/exactfloat/exactfloat.h"
#include "s2/util/math/vector.h"

// ---- smoke surface --------------------------------------------------------

extern "C" int s2bindings_s2_max_level(void) {
  return S2CellId::kMaxLevel;
}

extern "C" double s2bindings_unit_point_norm(double lat_deg, double lng_deg) {
  const S2LatLng ll = S2LatLng::FromDegrees(lat_deg, lng_deg);
  const S2Point p = ll.ToPoint();
  return p.Norm();
}

// ---- spherical-geometry kernel --------------------------------------------

// One opaque handle owns one S2Polygon. Allocated with `new`, released with
// s2bindings_polygon_free (`delete`).
struct s2bindings_polygon {
  S2Polygon poly;
};

namespace {

// Copies `msg` into the caller's error buffer, NUL-terminated and truncated to
// fit. No-op if the buffer is absent.
void write_err(char *buf, size_t len, const std::string &msg) {
  if (buf == nullptr || len == 0) return;
  const size_t n = std::min(len - 1, msg.size());
  std::memcpy(buf, msg.data(), n);
  buf[n] = '\0';
}

bool loop_index_in_range(const s2bindings_polygon *p, int loop_index) {
  return p != nullptr && loop_index >= 0 &&
         loop_index < p->poly.num_loops();
}

}  // namespace

extern "C" s2bindings_polygon *s2bindings_polygon_new(const double *lat_deg,
                                                      const double *lng_deg,
                                                      size_t n, char *err_buf,
                                                      size_t err_buf_len) {
  if (lat_deg == nullptr || lng_deg == nullptr) {
    write_err(err_buf, err_buf_len, "null coordinate array");
    return nullptr;
  }
  if (n < 3) {
    write_err(err_buf, err_buf_len,
              "polygon loop needs at least 3 distinct vertices");
    return nullptr;
  }

  std::vector<S2Point> points;
  points.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // S2LatLng takes (latitude, longitude). Non-finite inputs flow through as
    // non-unit-length points and are rejected by FindValidationError below.
    points.push_back(S2LatLng::FromDegrees(lat_deg[i], lng_deg[i]).ToPoint());
  }

  // Build with debug checks disabled so invalid input is reported via
  // FindValidationError() rather than aborting.
  auto loop = std::make_unique<S2Loop>(points, S2Debug::DISABLE);
  S2Error error;
  if (loop->FindValidationError(&error)) {
    write_err(err_buf, err_buf_len, error.text());
    return nullptr;
  }
  // Orient so the enclosed area is at most a hemisphere; input winding order is
  // therefore irrelevant.
  loop->Normalize();

  auto *handle = new s2bindings_polygon();
  handle->poly.set_s2debug_override(S2Debug::DISABLE);
  handle->poly.Init(std::move(loop));
  return handle;
}

extern "C" void s2bindings_polygon_free(s2bindings_polygon *p) {
  delete p;
}

extern "C" double s2bindings_polygon_area(const s2bindings_polygon *p) {
  return p == nullptr ? 0.0 : p->poly.GetArea();
}

extern "C" int s2bindings_polygon_is_empty(const s2bindings_polygon *p) {
  return (p != nullptr && p->poly.is_empty()) ? 1 : 0;
}

extern "C" int s2bindings_polygon_num_loops(const s2bindings_polygon *p) {
  return p == nullptr ? 0 : p->poly.num_loops();
}

extern "C" int s2bindings_polygon_loop_num_vertices(const s2bindings_polygon *p,
                                                    int loop_index) {
  if (!loop_index_in_range(p, loop_index)) return -1;
  return p->poly.loop(loop_index)->num_vertices();
}

extern "C" int s2bindings_polygon_loop_is_hole(const s2bindings_polygon *p,
                                               int loop_index) {
  if (!loop_index_in_range(p, loop_index)) return -1;
  return p->poly.loop(loop_index)->is_hole() ? 1 : 0;
}

extern "C" void s2bindings_polygon_loop_vertices(const s2bindings_polygon *p,
                                                 int loop_index,
                                                 double *lat_deg_out,
                                                 double *lng_deg_out) {
  if (!loop_index_in_range(p, loop_index)) return;
  if (lat_deg_out == nullptr || lng_deg_out == nullptr) return;
  const S2Loop *loop = p->poly.loop(loop_index);
  for (int i = 0; i < loop->num_vertices(); ++i) {
    const S2LatLng ll(loop->vertex(i));
    lat_deg_out[i] = ll.lat().degrees();
    lng_deg_out[i] = ll.lng().degrees();
  }
}

extern "C" s2bindings_polygon *s2bindings_polygon_intersection(
    const s2bindings_polygon *a, const s2bindings_polygon *b, char *err_buf,
    size_t err_buf_len) {
  if (a == nullptr || b == nullptr) {
    write_err(err_buf, err_buf_len, "null polygon argument");
    return nullptr;
  }

  auto *handle = new s2bindings_polygon();
  handle->poly.set_s2debug_override(S2Debug::DISABLE);
  S2Error error;
  // Default S2 tolerance posture: snap intersection vertices at
  // kIntersectionMergeRadius (~1.8e-15 rad), the minimum that guarantees a
  // topologically valid result while keeping vertices essentially exact.
  const bool ok = handle->poly.InitToIntersection(
      a->poly, b->poly,
      s2builderutil::IdentitySnapFunction(S2::kIntersectionMergeRadius),
      &error);
  if (!ok) {
    write_err(err_buf, err_buf_len, error.text());
    delete handle;
    return nullptr;
  }
  return handle;
}

// ---- spherical Delaunay / Voronoi connectivity ----------------------------

// A spherical Delaunay triangulation: the faces of the 3-D convex hull of the
// generator points (taken as unit vectors), plus the dual Voronoi vertices.
struct s2bindings_delaunay {
  std::vector<S2Point> points;             // generators, input order
  std::vector<std::array<int, 3>> tris;    // CCW-outward, canonicalized + sorted
  std::vector<S2Point> circumcenters;      // one unit point per triangle
};

namespace {

// Robust orientation predicate: the sign of det[b - a, c - a, d - a] (the
// signed volume of the tetrahedron, == (b-a) . ((c-a) x (d-a))). A value > 0
// means `d` lies on the OUTWARD side of the face (a, b, c) when that face is
// oriented CCW as seen from outside the sphere -- i.e. `d` is "visible" from
// the face and the hull must be extended toward it.
//
// Computed first in double precision, guarded by Shewchuk's static error filter
// (orient3d, "errboundA"); when the filtered result is not provably nonzero it
// falls back to ExactFloat arbitrary precision. Returns 0 only when the four
// points are EXACTLY coplanar -- a measure-zero case that does not arise for
// coordinates derived from lon/lat via trigonometry.
int orient3d(const S2Point& a, const S2Point& b, const S2Point& c,
             const S2Point& d) {
  const Vector3_d u = b - a, v = c - a, w = d - a;
  // Cofactor expansion of u . (v x w) and the matching permanent (the sum of
  // the magnitudes of every product that enters the determinant).
  const double m0 = v[1] * w[2] - v[2] * w[1];
  const double m1 = v[2] * w[0] - v[0] * w[2];
  const double m2 = v[0] * w[1] - v[1] * w[0];
  const double det = u[0] * m0 + u[1] * m1 + u[2] * m2;
  const double permanent =
      (std::abs(v[1] * w[2]) + std::abs(v[2] * w[1])) * std::abs(u[0]) +
      (std::abs(v[2] * w[0]) + std::abs(v[0] * w[2])) * std::abs(u[1]) +
      (std::abs(v[0] * w[1]) + std::abs(v[1] * w[0])) * std::abs(u[2]);
  // Shewchuk o3derrboundA = (7 + 56*eps) * eps, eps = 2^-53.
  const double errbound = 7.7715611723761e-16 * permanent;
  if (det > errbound) return 1;
  if (det < -errbound) return -1;

  // Inconclusive in double precision: recompute exactly. ExactFloat represents
  // every double exactly and its +,-,* are exact, so the determinant of the
  // exactly-converted points -- and hence its sign -- is exact.
  auto exact = [](const S2Point& p) {
    return Vector3<ExactFloat>(ExactFloat(p.x()), ExactFloat(p.y()),
                               ExactFloat(p.z()));
  };
  const Vector3<ExactFloat> ea = exact(a), eb = exact(b), ec = exact(c),
                            ed = exact(d);
  const ExactFloat edet = (eb - ea).DotProd((ec - ea).CrossProd(ed - ea));
  return edet.sgn();
}

// One triangular face of the hull, vertices in CCW-outward order.
struct Face {
  int a, b, c;
};

// Builds the convex-hull faces of `pts` (unit vectors) into `faces`. Returns
// false and sets `err` on degenerate input. Deterministic: the convex hull is
// unique, and all orientation decisions use the exact `orient3d` predicate, so
// the face set does not depend on insertion order.
bool build_hull(const std::vector<S2Point>& pts, std::vector<Face>& faces,
                std::string& err) {
  const int n = static_cast<int>(pts.size());
  if (n < 4) {
    err = "spherical Delaunay needs at least 4 generator points";
    return false;
  }

  // --- Seed tetrahedron: the first four affinely-independent generators. ---
  int i0 = 0, i1 = -1, i2 = -1, i3 = -1;
  for (int i = 1; i < n; ++i) {
    if (pts[i] != pts[i0]) {
      i1 = i;
      break;
    }
  }
  if (i1 < 0) {
    err = "all generator points are identical";
    return false;
  }
  // i2: not collinear with (i0, i1) in 3-D.
  for (int i = i1 + 1; i < n; ++i) {
    if ((pts[i1] - pts[i0]).CrossProd(pts[i] - pts[i0]).Norm2() > 0) {
      i2 = i;
      break;
    }
  }
  if (i2 < 0) {
    err = "all generator points are collinear";
    return false;
  }
  // i3: forms a non-degenerate tetrahedron with (i0, i1, i2).
  for (int i = i2 + 1; i < n; ++i) {
    if (orient3d(pts[i0], pts[i1], pts[i2], pts[i]) != 0) {
      i3 = i;
      break;
    }
  }
  if (i3 < 0) {
    err = "all generator points are coplanar; a 3-D hull is undefined";
    return false;
  }

  // Orient each seed face so that the opposite (apex) vertex lies on its INNER
  // side: orient3d(face, apex) < 0. This gives the seed tetrahedron a
  // consistent outward orientation WITHOUT assuming the partial hull encloses
  // the origin yet -- e.g. a seed with two antipodal vertices has a face plane
  // through the origin, where an origin-based test would be ambiguous.
  auto oriented = [&](int x, int y, int z, int apex) -> Face {
    if (orient3d(pts[x], pts[y], pts[z], pts[apex]) < 0) return Face{x, y, z};
    return Face{x, z, y};
  };
  faces.clear();
  faces.push_back(oriented(i0, i1, i2, i3));
  faces.push_back(oriented(i0, i1, i3, i2));
  faces.push_back(oriented(i0, i2, i3, i1));
  faces.push_back(oriented(i1, i2, i3, i0));

  auto is_seed_idx = [&](int i) {
    return i == i0 || i == i1 || i == i2 || i == i3;
  };

  // --- Incremental insertion of the remaining generators, in input order. ---
  // Every distinct point on the sphere lies strictly outside the convex hull of
  // any subset of the other sphere points, so each new generator is guaranteed
  // to see at least one face.
  for (int p = 0; p < n; ++p) {
    if (is_seed_idx(p)) continue;

    // Directed edges of the visible faces; the horizon is the set of directed
    // edges whose reverse is not itself on a visible face.
    std::map<std::pair<int, int>, bool> vis_edges;
    std::vector<Face> kept;
    kept.reserve(faces.size());
    bool any_visible = false;
    for (const Face& f : faces) {
      if (orient3d(pts[f.a], pts[f.b], pts[f.c], pts[p]) > 0) {
        any_visible = true;
        vis_edges[{f.a, f.b}] = true;
        vis_edges[{f.b, f.c}] = true;
        vis_edges[{f.c, f.a}] = true;
      } else {
        kept.push_back(f);
      }
    }
    if (!any_visible) {
      // Unreachable for distinct sphere points; guard against duplicates that
      // slipped past the seed check.
      err = "duplicate or degenerate generator point";
      return false;
    }

    // A directed edge (u, v) is on the horizon iff (v, u) is not also visible.
    for (const auto& kv : vis_edges) {
      const int u = kv.first.first, v = kv.first.second;
      if (vis_edges.find({v, u}) == vis_edges.end()) {
        // The surviving neighbour across {u, v} carries the edge as (v, u), so
        // the new face must carry it as (u, v) to stay outward-CCW.
        kept.push_back(Face{u, v, p});
      }
    }
    faces.swap(kept);
  }

  // Euler check: a triangulated sphere on n vertices has exactly 2n - 4 faces.
  // Any other count means the construction produced an invalid mesh (which can
  // only happen for an exactly-degenerate input).
  if (static_cast<int>(faces.size()) != 2 * n - 4) {
    err = "degenerate generator configuration (triangulation is not a "
          "topological sphere)";
    return false;
  }

  // Re-orient every face CCW as seen from outside the sphere. For a closed
  // global mesh the origin lies strictly inside the hull, so a face (a, b, c) is
  // outward-CCW iff det[a, b, c] = s2pred::Sign(a, b, c) > 0 (exact). This makes
  // the whole surface consistently oriented for the canonical output and the
  // dual fan-walk. A face whose plane passes through the origin (Sign == 0)
  // means the generators do not enclose the sphere centre -- not a closed mesh.
  for (Face& f : faces) {
    const int s = s2pred::Sign(pts[f.a], pts[f.b], pts[f.c]);
    if (s == 0) {
      err = "generators do not enclose the sphere centre (a hull face passes "
            "through the origin); a closed global mesh is required";
      return false;
    }
    if (s < 0) std::swap(f.b, f.c);
  }
  return true;
}

// Circumcenter (dual Voronoi vertex) of an outward-CCW triangle: the unit point
// equidistant from a, b, c, i.e. the normalized outward normal of their plane.
S2Point circumcenter(const S2Point& a, const S2Point& b, const S2Point& c) {
  S2Point n = (b - a).CrossProd(c - a);
  // For an outward-CCW face the normal already points away from the origin
  // (n . a > 0); guard the sign so the dual vertex is the OUTWARD circumcenter
  // regardless, then normalize onto the unit sphere.
  if (n.DotProd(a) < 0) n = -n;
  return n.Normalize();
}

}  // namespace

extern "C" s2bindings_delaunay* s2bindings_delaunay_new(const double* lat_deg,
                                                        const double* lng_deg,
                                                        size_t n, char* err_buf,
                                                        size_t err_buf_len) {
  if (lat_deg == nullptr || lng_deg == nullptr) {
    write_err(err_buf, err_buf_len, "null coordinate array");
    return nullptr;
  }
  if (n < 4) {
    write_err(err_buf, err_buf_len,
              "spherical Delaunay needs at least 4 generator points");
    return nullptr;
  }

  std::vector<S2Point> points;
  points.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(lat_deg[i]) || !std::isfinite(lng_deg[i])) {
      write_err(err_buf, err_buf_len, "non-finite generator coordinate");
      return nullptr;
    }
    points.push_back(S2LatLng::FromDegrees(lat_deg[i], lng_deg[i]).ToPoint());
  }

  std::vector<Face> faces;
  std::string err;
  if (!build_hull(points, faces, err)) {
    write_err(err_buf, err_buf_len, err);
    return nullptr;
  }

  // Canonicalize each triangle: rotate so the smallest cell index is first
  // (preserving CCW orientation), then sort the list lexicographically. This
  // makes the integer triangle list a deterministic function of the inputs.
  std::vector<std::array<int, 3>> tris;
  tris.reserve(faces.size());
  for (const Face& f : faces) {
    std::array<int, 3> t;
    if (f.a <= f.b && f.a <= f.c) {
      t = {f.a, f.b, f.c};
    } else if (f.b <= f.a && f.b <= f.c) {
      t = {f.b, f.c, f.a};
    } else {
      t = {f.c, f.a, f.b};
    }
    tris.push_back(t);
  }
  std::sort(tris.begin(), tris.end());

  auto* handle = new s2bindings_delaunay();
  handle->points = std::move(points);
  handle->tris = std::move(tris);
  handle->circumcenters.reserve(handle->tris.size());
  for (const auto& t : handle->tris) {
    handle->circumcenters.push_back(circumcenter(
        handle->points[t[0]], handle->points[t[1]], handle->points[t[2]]));
  }
  return handle;
}

extern "C" void s2bindings_delaunay_free(s2bindings_delaunay* d) { delete d; }

extern "C" int s2bindings_delaunay_num_points(const s2bindings_delaunay* d) {
  return d == nullptr ? 0 : static_cast<int>(d->points.size());
}

extern "C" int s2bindings_delaunay_num_triangles(const s2bindings_delaunay* d) {
  return d == nullptr ? 0 : static_cast<int>(d->tris.size());
}

extern "C" void s2bindings_delaunay_triangles(const s2bindings_delaunay* d,
                                              int* out_ijk) {
  if (d == nullptr || out_ijk == nullptr) return;
  for (size_t t = 0; t < d->tris.size(); ++t) {
    out_ijk[3 * t + 0] = d->tris[t][0];
    out_ijk[3 * t + 1] = d->tris[t][1];
    out_ijk[3 * t + 2] = d->tris[t][2];
  }
}

extern "C" void s2bindings_delaunay_circumcenters(const s2bindings_delaunay* d,
                                                  double* lat_deg_out,
                                                  double* lng_deg_out) {
  if (d == nullptr || lat_deg_out == nullptr || lng_deg_out == nullptr) return;
  for (size_t t = 0; t < d->circumcenters.size(); ++t) {
    const S2LatLng ll(d->circumcenters[t]);
    lat_deg_out[t] = ll.lat().degrees();
    lng_deg_out[t] = ll.lng().degrees();
  }
}
