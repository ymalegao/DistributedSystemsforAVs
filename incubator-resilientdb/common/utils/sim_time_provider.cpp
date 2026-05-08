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

#include "common/utils/sim_time_provider.h"

namespace resdb {

std::atomic<uint64_t> SimTimeProvider::now_us_{0};
std::mutex SimTimeProvider::mutex_;
std::condition_variable SimTimeProvider::cv_;

uint64_t SimTimeProvider::NowUs() { return now_us_.load(std::memory_order_relaxed); }

void SimTimeProvider::UpdateNowUs(uint64_t now_us) {
  uint64_t prev = now_us_.load(std::memory_order_relaxed);
  if (now_us <= prev) {
    // Still wake waiters; they may be waiting for "any tick" rather than
    // strictly increasing time.
    cv_.notify_all();
    return;
  }
  now_us_.store(now_us, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(mutex_);
  cv_.notify_all();
}

void SimTimeProvider::SleepUntilUs(uint64_t deadline_us) {
  if (NowUs() >= deadline_us) {
    return;
  }
  std::unique_lock<std::mutex> lk(mutex_);
  cv_.wait(lk, [&]() { return NowUs() >= deadline_us; });
}

void SimTimeProvider::SleepForUs(uint64_t delta_us) {
  uint64_t now = NowUs();
  SleepUntilUs(now + delta_us);
}

}  // namespace resdb

