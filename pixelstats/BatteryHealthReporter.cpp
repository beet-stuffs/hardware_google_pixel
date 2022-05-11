/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "pixelstats: BatteryHealthReporter"

#include <android-base/file.h>
#include <hardware/google/pixel/pixelstats/pixelatoms.pb.h>
#include <log/log.h>
#include <pixelstats/BatteryHealthReporter.h>
#include <pixelstats/StatsHelper.h>
#include <time.h>
#include <utils/Timers.h>

#include <cinttypes>

namespace android {
namespace hardware {
namespace google {
namespace pixel {

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
using aidl::android::frameworks::stats::VendorAtomValue;
using android::base::ReadFileToString;
using android::base::WriteStringToFile;
using android::hardware::google::pixel::PixelAtoms::BatteryHealthStatus;

const int SECONDS_PER_MONTH = 60 * 60 * 24 * 30;

BatteryHealthReporter::BatteryHealthReporter() {}

int64_t BatteryHealthReporter::getTimeSecs(void) {
    return nanoseconds_to_seconds(systemTime(SYSTEM_TIME_BOOTTIME));
}

void BatteryHealthReporter::reportBatteryHealthStatus(const std::shared_ptr<IStats> &stats_client,
                                                      const char *line) {
    int health_status_stats_fields[] = {
            BatteryHealthStatus::kHealthAlgorithmFieldNumber,
            BatteryHealthStatus::kHealthStatusFieldNumber,
            BatteryHealthStatus::kHealthIndexFieldNumber,
            BatteryHealthStatus::kHealthCapacityIndexFieldNumber,
            BatteryHealthStatus::kHealthPerfIndexFieldNumber,
            BatteryHealthStatus::kSwellingCumulativeFieldNumber,
            BatteryHealthStatus::kHealthFullCapacityFieldNumber,
            BatteryHealthStatus::kCurrentImpedanceFieldNumber,
            BatteryHealthStatus::kBatteryAgeFieldNumber,
            BatteryHealthStatus::kCycleCountFieldNumber,
    };

    const int32_t vtier_fields_size = std::size(health_status_stats_fields);
    static_assert(vtier_fields_size == 10, "Unexpected battery health status fields size");
    std::vector<VendorAtomValue> values(vtier_fields_size);
    VendorAtomValue val;
    int32_t i = 0, tmp[vtier_fields_size] = {0};

    // health_algo: health_status, health_index,healh_capacity_index,health_perf_index,
    // swelling_cumulative,health_full_capacity,current_impedance, battery_age,cycle_count
    if (sscanf(line, "%d: %d, %d,%d,%d %d,%d,%d %d,%d", &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4],
               &tmp[5], &tmp[6], &tmp[7], &tmp[8], &tmp[9]) != vtier_fields_size) {
        /* If format isn't as expected, then ignore line on purpose */
        return;
    }

    ALOGD("BatteryHealthStatus: processed %s", line);
    for (i = 0; i < vtier_fields_size; i++) {
        val.set<VendorAtomValue::intValue>(tmp[i]);
        values[health_status_stats_fields[i] - kVendorAtomOffset] = val;
    }

    VendorAtom event = {.reverseDomainName = "",
                        .atomId = PixelAtoms::Atom::kBatteryHealthStatus,
                        .values = std::move(values)};
    const ndk::ScopedAStatus ret = stats_client->reportVendorAtom(event);
    if (!ret.isOk())
        ALOGE("Unable to report BatteryHealthStatus to Stats service");
}

void BatteryHealthReporter::checkAndReportStatus(const std::shared_ptr<IStats> &stats_client) {
    std::string file_contents, line;
    std::istringstream ss;
    std::string path = kBatteryHealthStatusPath;

    int64_t now = getTimeSecs();
    if ((report_time_ != 0) && (now - report_time_ < SECONDS_PER_MONTH)) {
        ALOGD("Do not upload yet. now: %" PRId64 ", pre: %" PRId64, now, report_time_);
        return;
    }

    if (!ReadFileToString(path.c_str(), &file_contents)) {
        ALOGE("Unable to read %s - %s", path.c_str(), strerror(errno));
        return;
    }

    ss.str(file_contents);

    report_time_ = now;
    while (std::getline(ss, line)) {
        reportBatteryHealthStatus(stats_client, line.c_str());
    }
}

}  // namespace pixel
}  // namespace google
}  // namespace hardware
}  // namespace android
