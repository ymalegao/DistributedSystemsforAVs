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

#include "common/utils/utils.h"

#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/utils/sim_time_provider.h"

namespace resdb {

#define CPU_FREQ 2.2
#define TIME_ENABLE true

uint64_t GetCurrentTime() {
  // Simulation mode: if the OMNeT++ layer injected a sim-time value, use it.
  uint64_t sim_now = SimTimeProvider::NowUs();
  if (sim_now != 0) {
    return sim_now;
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)(tv.tv_sec) * 1000000 + tv.tv_usec;
}

void SleepForUs(uint64_t delta_us) {
  // If sim-time is active, block on the sim-time condition variable instead
  // of wall-clock sleep.
  if (SimTimeProvider::NowUs() != 0) {
    SimTimeProvider::SleepForUs(delta_us);
    return;
  }
  usleep(delta_us);
}

}  // namespace resdb
