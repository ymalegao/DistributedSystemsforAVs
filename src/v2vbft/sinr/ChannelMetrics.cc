// Copyright (C) 2026 Mathesh Kumar
// SPDX-License-Identifier: GPL-3.0-or-later

#include "v2vbft/sinr/ChannelMetrics.h"
#include "veins/modules/mac/ieee80211p/Mac1609_4.h"
#include <cmath>
#include <iostream>

namespace v2vbft {

ChannelMetrics::ChannelMetrics(int vehicleId,
                               const std::string& utilizationCsvPath,
                               const std::string& sinrCsvPath)
    : vehicleId_(vehicleId)
    , busyAccum_(SIMTIME_ZERO)
    , busyStart_(SIMTIME_ZERO)
{
    csv_ = fopen(utilizationCsvPath.c_str(), "w");
    if (csv_) {
        fprintf(csv_, "time_s,vehicle_id,channel_utilization\n");
        fflush(csv_);
    }

    sinrCsv_ = fopen(sinrCsvPath.c_str(), "w");
    if (sinrCsv_) {
        fprintf(sinrCsv_, "time_s,vehicle_id,sinr_db_mean,sinr_db_min\n");
        fflush(sinrCsv_);
    }
}

ChannelMetrics::~ChannelMetrics()
{
    if (csv_)     { fclose(csv_);     csv_     = nullptr; }
    if (sinrCsv_) { fclose(sinrCsv_); sinrCsv_ = nullptr; }
}

void ChannelMetrics::receiveSignal(cComponent*, simsignal_t sig, bool value, cObject*)
{
    // MAC collision (hidden-terminal / simultaneous TX) — confirms air loss that
    // never shows up in RX-only SINR CSVs.
    if (sig == veins::Mac1609_4::sigCollision) {
        std::cout << "[MAC-COLLISION] r" << vehicleId_ << " t=" << simTime() << "\n";
        return;
    }

    const bool isBusy = value;
    simtime_t now = simTime();
    if (isBusy && !channelBusy_) {
        busyStart_ = now;
        channelBusy_ = true;
    } else if (!isBusy && channelBusy_) {
        busyAccum_ += now - busyStart_;
        channelBusy_ = false;
    }
}

void ChannelMetrics::addSinrSample(double sinr_linear)
{
    if (sinr_linear <= 0.0) return;
    double sinr_db = 10.0 * std::log10(sinr_linear);
    sinrSum_ += sinr_db;
    if (sinr_db < sinrMin_) sinrMin_ = sinr_db;
    sinrSamples_++;
}

void ChannelMetrics::tick(simtime_t now)
{
    // ---- Channel utilization ----
    simtime_t accum = busyAccum_;
    if (channelBusy_) accum += now - busyStart_;
    double utilization = accum.dbl() / UTILIZATION_WINDOW_SEC;
    if (utilization > 1.0) utilization = 1.0;
    if (csv_) {
        fprintf(csv_, "%.3f,%d,%.4f\n", now.dbl(), vehicleId_, utilization);
        fflush(csv_);
    }
    // Fleet-representative util snapshot (r0 only) — greppable against PREPARE air_t.
    // SINR CSVs alone are misleading: they only sample successfully decoded frames.
    if (vehicleId_ == 0) {
        std::cout << "[CHAN-UTIL] r0 util=" << utilization << " t=" << now << "\n";
    }
    busyAccum_ = SIMTIME_ZERO;
    if (channelBusy_) busyStart_ = now;

    // ---- SINR ----
    if (sinrCsv_) {
        if (sinrSamples_ > 0) {
            double mean_db = sinrSum_ / sinrSamples_;
            double min_db  = sinrMin_;
            fprintf(sinrCsv_, "%.3f,%d,%.4f,%.4f\n",
                    now.dbl(), vehicleId_, mean_db, min_db);
        }
        // Write nothing when no packets were received this window (sparse CSV).
        fflush(sinrCsv_);
    }
    sinrSum_     = 0.0;
    sinrMin_     = std::numeric_limits<double>::infinity();
    sinrSamples_ = 0;
}

} // namespace v2vbft
