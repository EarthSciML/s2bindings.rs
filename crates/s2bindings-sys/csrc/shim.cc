// C ABI shim over the s2geometry C++ library. See shim.h for the contract.

#include "shim.h"

#include "s2/s2cell_id.h"
#include "s2/s2latlng.h"
#include "s2/s2point.h"

extern "C" int s2bindings_s2_max_level(void) {
  return S2CellId::kMaxLevel;
}

extern "C" double s2bindings_unit_point_norm(double lat_deg, double lng_deg) {
  const S2LatLng ll = S2LatLng::FromDegrees(lat_deg, lng_deg);
  const S2Point p = ll.ToPoint();
  return p.Norm();
}
