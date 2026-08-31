#include "integration/omnet/resdb_intersection_scheduler.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "gtest/gtest.h"

namespace resdb::omnet {
namespace {

ResdbVehicleEntry Entry(int32_t replica_id, uint8_t lane,
                        uint8_t physical_lane, uint8_t position,
                        bool ambulance = false) {
  ResdbVehicleEntry entry{};
  entry.replica_id = replica_id;
  entry.sim_time_us = 1;
  entry.is_ambulance = ambulance ? 1 : 0;
  entry.lane = lane;
  entry.direction = 3;  // UNKNOWN keeps every vehicle in a singleton batch.
  entry.position_in_lane = position;
  entry.cyber_status = 1;
  entry.physical_lane_index = physical_lane;
  entry.lateral_claim_cm = physical_lane == 0 ? 0 : 320;
  return entry;
}

std::map<int32_t, uint32_t> DecodeBatches(
    const IntersectionScheduleResult& result) {
  EXPECT_GE(result.order_bytes.size(), sizeof(ResdbOrderHdr));
  ResdbOrderHdr header{};
  std::memcpy(&header, result.order_bytes.data(), sizeof(header));
  EXPECT_EQ(result.order_bytes.size(),
            sizeof(header) +
                header.n_vehicles * sizeof(ResdbVehicleDecision));

  std::map<int32_t, uint32_t> batches;
  const char* cursor = result.order_bytes.data() + sizeof(header);
  for (uint32_t i = 0; i < header.n_vehicles; ++i) {
    ResdbVehicleDecision decision{};
    std::memcpy(&decision, cursor, sizeof(decision));
    cursor += sizeof(decision);
    batches[decision.replica_id] = decision.batch_index;
  }
  return batches;
}

TEST(IntersectionSchedulerTest,
     AmbulanceWaitsOnlyForItsPhysicalLaneBlockers) {
  // East has two independent physical queues. veh7 and veh8 are ahead of the
  // outer-lane ambulance.  The certified rank deliberately misorders veh8
  // behind the late ambulance; it must still clear first because it was
  // already present in the same physical queue. veh6 and veh14 occupy the
  // inner lane and must not delay the ambulance.
  std::vector<ResdbVehicleEntry> entries = {
      Entry(14, 2, 1, 2),
      Entry(17, 2, 0, 1, true),
      Entry(6, 2, 1, 1),
      Entry(8, 2, 0, 2),
      Entry(7, 2, 0, 1),
      Entry(1, 0, 0, 1),
  };
  ResdbProposeHdr header{};
  header.n_vehicles = entries.size();

  const auto batches = DecodeBatches(
      BuildIntersectionSchedule(header, entries));

  EXPECT_LT(batches.at(7), batches.at(17));
  EXPECT_LT(batches.at(8), batches.at(17));
  EXPECT_EQ(batches.at(17), 2u);
  EXPECT_GT(batches.at(6), batches.at(17));
  EXPECT_GT(batches.at(14), batches.at(17));
}

TEST(IntersectionSchedulerTest, FrontPrecedesRearWithinPhysicalLane) {
  // Input order is deliberately rear-first. The inner-lane car is independent,
  // while the south outer-lane front must still precede its rear follower.
  std::vector<ResdbVehicleEntry> entries = {
      Entry(5, 1, 0, 2),
      Entry(4, 1, 0, 1),
      Entry(3, 1, 1, 1),
  };
  ResdbProposeHdr header{};
  header.n_vehicles = entries.size();

  const auto batches = DecodeBatches(
      BuildIntersectionSchedule(header, entries));

  EXPECT_LT(batches.at(4), batches.at(5));
}

}  // namespace
}  // namespace resdb::omnet
