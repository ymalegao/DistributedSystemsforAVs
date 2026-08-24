#pragma once

#include <string>
#include <vector>

#include "integration/omnet/resdb_omnet_bridge.h"

namespace resdb::omnet {

struct IntersectionScheduleResult {
  std::string order_bytes;
  uint32_t n_batches = 0;
  int ambulance_lane = -1;
};

IntersectionScheduleResult BuildIntersectionSchedule(
    const ResdbProposeHdr& hdr,
    const std::vector<ResdbVehicleEntry>& entries);

}  // namespace resdb::omnet
