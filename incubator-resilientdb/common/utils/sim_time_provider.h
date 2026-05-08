/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>

namespace resdb {

// SimTimeProvider virtualizes "now" for deterministic simulation environments
// (e.g., OMNeT++). The simulation thread periodically calls UpdateNowUs().
// Other ResDB worker threads read NowUs() and wait using Sleep*() without
// touching wall-clock timers.
class SimTimeProvider {
 public:
  // Returns the latest injected simulation time in microseconds.
  // If no time was ever injected, returns 0.
  static uint64_t NowUs();

  // Called by the simulation thread to advance time and wake waiters.
  static void UpdateNowUs(uint64_t now_us);

  // Blocks until NowUs() >= deadline_us. Returns immediately if already past.
  static void SleepUntilUs(uint64_t deadline_us);

  // Convenience helper.
  static void SleepForUs(uint64_t delta_us);

 private:
  static std::atomic<uint64_t> now_us_;
  static std::mutex mutex_;
  static std::condition_variable cv_;
};

}  // namespace resdb

