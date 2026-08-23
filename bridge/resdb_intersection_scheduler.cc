// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

#include "integration/omnet/resdb_intersection_scheduler.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <unordered_set>

namespace resdb::omnet {
namespace {

bool IsSafeToBatch(uint8_t lane_a, uint8_t dir_a,
                   uint8_t lane_b, uint8_t dir_b) {
  if (lane_a == lane_b) return false;
  static const uint8_t kSafe[12][4] = {
      {0, 0, 1, 0}, {2, 0, 3, 0}, {0, 2, 1, 2}, {0, 2, 2, 2},
      {0, 2, 3, 2}, {1, 2, 2, 2}, {1, 2, 3, 2}, {2, 2, 3, 2},
      {0, 2, 1, 0}, {1, 2, 0, 0}, {2, 2, 3, 0}, {3, 2, 2, 0},
  };
  for (const auto& p : kSafe) {
    if ((lane_a == p[0] && dir_a == p[1] && lane_b == p[2] && dir_b == p[3]) ||
        (lane_a == p[2] && dir_a == p[3] && lane_b == p[0] && dir_b == p[1])) {
      return true;
    }
  }
  return false;
}

bool IsQuietEntry(const ResdbVehicleEntry& e) {
  return e.cyber_status == 0 || e.sim_time_us == UINT64_MAX;
}

int CompareLaneQueueOrder(const ResdbVehicleEntry& a,
                          const ResdbVehicleEntry& b) {
  if (a.position_in_lane != b.position_in_lane)
    return static_cast<int>(a.position_in_lane) - static_cast<int>(b.position_in_lane);
  if (a.replica_id < b.replica_id) return -1;
  if (a.replica_id > b.replica_id) return 1;
  return 0;
}

bool AllSameLaneFrontPlaced(const ResdbVehicleEntry& candidate,
                            const std::vector<ResdbVehicleEntry>& view,
                            const std::unordered_set<int32_t>& placed) {
  for (const auto& v : view) {
    if (v.lane != candidate.lane) continue;
    if (CompareLaneQueueOrder(v, candidate) < 0 &&
        placed.find(v.replica_id) == placed.end()) {
      return false;
    }
  }
  return true;
}

bool SafeWithWholeBatch(const ResdbVehicleEntry& e,
                        const std::vector<ResdbVehicleEntry>& batch) {
  if (IsQuietEntry(e)) return false;
  for (const auto& b : batch) {
    if (!IsSafeToBatch(e.lane, e.direction, b.lane, b.direction)) return false;
  }
  return true;
}

}  // namespace

IntersectionScheduleResult BuildIntersectionSchedule(
    const ResdbProposeHdr& hdr,
    const std::vector<ResdbVehicleEntry>& entries) {
  IntersectionScheduleResult result;

  for (const auto& e : entries) {
    if (e.is_ambulance && e.cyber_status == 1) {
      result.ambulance_lane = static_cast<int>(e.lane);
      break;
    }
  }

  std::vector<ResdbVehicleEntry> ambulances;
  for (const auto& e : entries) {
    if (e.is_ambulance && e.cyber_status == 1) ambulances.push_back(e);
  }
  std::sort(ambulances.begin(), ambulances.end(),
            [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
              if (a.position_in_lane != b.position_in_lane)
                return a.position_in_lane < b.position_in_lane;
              return a.replica_id < b.replica_id;
            });

  std::unordered_set<int32_t> priority_ids;
  std::vector<ResdbVehicleEntry> work_queue;
  for (const auto& ambulance : ambulances) {
    std::vector<ResdbVehicleEntry> blockers;
    for (const auto& v : entries) {
      if (v.lane != ambulance.lane || v.is_ambulance) continue;
      if (CompareLaneQueueOrder(v, ambulance) < 0) blockers.push_back(v);
    }
    std::sort(blockers.begin(), blockers.end(),
              [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
                if (a.position_in_lane != b.position_in_lane)
                  return a.position_in_lane < b.position_in_lane;
                return a.replica_id < b.replica_id;
              });
    for (const auto& blocker : blockers) {
      if (priority_ids.insert(blocker.replica_id).second)
        work_queue.push_back(blocker);
    }
    if (priority_ids.insert(ambulance.replica_id).second)
      work_queue.push_back(ambulance);
  }

  std::vector<ResdbVehicleEntry> remaining_entries;
  for (const auto& e : entries) {
    if (priority_ids.find(e.replica_id) == priority_ids.end())
      remaining_entries.push_back(e);
  }
  std::sort(remaining_entries.begin(), remaining_entries.end(),
            [](const ResdbVehicleEntry& a, const ResdbVehicleEntry& b) {
              if (a.position_in_lane != b.position_in_lane)
                return a.position_in_lane < b.position_in_lane;
              if (a.replica_id != b.replica_id) return a.replica_id < b.replica_id;
              return a.sim_time_us < b.sim_time_us;
            });
  for (const auto& e : remaining_entries) work_queue.push_back(e);

  const uint32_t n = static_cast<uint32_t>(entries.size());
  std::vector<ResdbVehicleDecision> decisions(n);
  std::vector<std::vector<ResdbVehicleEntry>> batches_out;
  std::unordered_set<int32_t> placed;

  while (placed.size() < n) {
    const ResdbVehicleEntry* head_ptr = nullptr;
    for (const auto& cand : work_queue) {
      if (placed.count(cand.replica_id)) continue;
      if (!AllSameLaneFrontPlaced(cand, entries, placed)) continue;
      head_ptr = &cand;
      break;
    }
    if (!head_ptr) {
      std::cout << "[SCHEDULER] no schedulable head placed=" << placed.size()
                << "/" << n << "\n";
      for (const auto& e : entries) {
        if (placed.count(e.replica_id)) continue;
        batches_out.push_back({e});
        placed.insert(e.replica_id);
      }
      break;
    }

    ResdbVehicleEntry head = *head_ptr;
    std::vector<ResdbVehicleEntry> batch;
    batch.push_back(head);
    placed.insert(head.replica_id);

    if (IsQuietEntry(head)) {
      batches_out.push_back(std::move(batch));
      continue;
    }

    bool grew = false;
    do {
      grew = false;
      for (const auto& cand : work_queue) {
        if (placed.count(cand.replica_id)) continue;
        if (!AllSameLaneFrontPlaced(cand, entries, placed)) continue;
        if (IsQuietEntry(cand)) continue;
        if (!SafeWithWholeBatch(cand, batch)) continue;
        batch.push_back(cand);
        placed.insert(cand.replica_id);
        grew = true;
      }
    } while (grew);

    batches_out.push_back(std::move(batch));
  }

  result.n_batches = static_cast<uint32_t>(batches_out.size());
  for (uint32_t bi = 0; bi < batches_out.size(); ++bi) {
    for (const auto& b : batches_out[bi]) {
      for (uint32_t i = 0; i < n; ++i) {
        if (entries[i].replica_id == b.replica_id)
          decisions[i] = {b.replica_id, bi};
      }
    }
  }

  result.order_bytes.assign(sizeof(ResdbOrderHdr) +
                                n * sizeof(ResdbVehicleDecision),
                            '\0');
  uint8_t* out = reinterpret_cast<uint8_t*>(&result.order_bytes[0]);
  ResdbOrderHdr ohdr{hdr.epoch, n, result.n_batches};
  std::memcpy(out, &ohdr, sizeof(ohdr));
  out += sizeof(ohdr);
  for (uint32_t i = 0; i < n; ++i) {
    std::memcpy(out, &decisions[i], sizeof(ResdbVehicleDecision));
    out += sizeof(ResdbVehicleDecision);
  }

  return result;
}

}  // namespace resdb::omnet
