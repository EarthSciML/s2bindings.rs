// C ABI shim over the s2geometry C++ library. See shim.h for the contract.

#include "shim.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2cell_id.h"
#include "s2/s2edge_crossings.h"
#include "s2/s2error.h"
#include "s2/s2latlng.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"

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
