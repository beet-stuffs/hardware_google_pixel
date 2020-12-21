/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <iterator>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <hidl/HidlTransportSupport.h>

#include "thermal-helper.h"

namespace android {
namespace hardware {
namespace thermal {
namespace V2_0 {
namespace implementation {

constexpr std::string_view kCpuOnlineRoot("/sys/devices/system/cpu");
constexpr std::string_view kThermalSensorsRoot("/sys/devices/virtual/thermal");
constexpr std::string_view kCpuUsageFile("/proc/stat");
constexpr std::string_view kCpuOnlineFileSuffix("online");
constexpr std::string_view kCpuPresentFile("/sys/devices/system/cpu/present");
constexpr std::string_view kSensorPrefix("thermal_zone");
constexpr std::string_view kCoolingDevicePrefix("cooling_device");
constexpr std::string_view kThermalNameFile("type");
constexpr std::string_view kSensorPolicyFile("policy");
constexpr std::string_view kSensorTempSuffix("temp");
constexpr std::string_view kSensorTripPointTempZeroFile("trip_point_0_temp");
constexpr std::string_view kSensorTripPointHystZeroFile("trip_point_0_hyst");
constexpr std::string_view kUserSpaceSuffix("user_space");
constexpr std::string_view kCoolingDeviceCurStateSuffix("cur_state");
constexpr std::string_view kConfigProperty("vendor.thermal.config");
constexpr std::string_view kConfigDefaultFileName("thermal_info_config.json");

namespace {
using android::base::StringPrintf;
using android::hardware::thermal::V2_0::toString;

/*
 * Pixel don't offline CPU, so std::thread::hardware_concurrency(); should work.
 * However /sys/devices/system/cpu/present is preferred.
 * The file is expected to contain single text line with two numbers %d-%d,
 * which is a range of available cpu numbers, e.g. 0-7 would mean there
 * are 8 cores number from 0 to 7.
 * For Android systems this approach is safer than using cpufeatures, see bug
 * b/36941727.
 */
static int getNumberOfCores() {
    std::string file;
    if (!android::base::ReadFileToString(kCpuPresentFile.data(), &file)) {
        LOG(ERROR) << "Error reading Cpu present file: " << kCpuPresentFile;
        return 0;
    }
    std::vector<std::string> pieces = android::base::Split(file, "-");
    if (pieces.size() != 2) {
        LOG(ERROR) << "Error parsing Cpu present file content: " << file;
        return 0;
    }
    auto min_core = std::stoul(pieces[0]);
    auto max_core = std::stoul(pieces[1]);
    if (max_core < min_core) {
        LOG(ERROR) << "Error parsing Cpu present min and max: " << min_core << " - " << max_core;
        return 0;
    }
    return static_cast<std::size_t>(max_core - min_core + 1);
}
const int kMaxCpus = getNumberOfCores();

void parseCpuUsagesFileAndAssignUsages(hidl_vec<CpuUsage> *cpu_usages) {
    std::string data;
    if (!android::base::ReadFileToString(kCpuUsageFile.data(), &data)) {
        LOG(ERROR) << "Error reading cpu usage file: " << kCpuUsageFile;
        return;
    }

    std::istringstream stat_data(data);
    std::string line;
    while (std::getline(stat_data, line)) {
        if (line.find("cpu") == 0 && isdigit(line[3])) {
            // Split the string using spaces.
            std::vector<std::string> words = android::base::Split(line, " ");
            std::string cpu_name = words[0];
            int cpu_num = std::stoi(cpu_name.substr(3));

            if (cpu_num < kMaxCpus) {
                uint64_t user = std::stoull(words[1]);
                uint64_t nice = std::stoull(words[2]);
                uint64_t system = std::stoull(words[3]);
                uint64_t idle = std::stoull(words[4]);

                // Check if the CPU is online by reading the online file.
                std::string cpu_online_path =
                        StringPrintf("%s/%s/%s", kCpuOnlineRoot.data(), cpu_name.c_str(),
                                     kCpuOnlineFileSuffix.data());
                std::string is_online;
                if (!android::base::ReadFileToString(cpu_online_path, &is_online)) {
                    LOG(ERROR) << "Could not open Cpu online file: " << cpu_online_path;
                    if (cpu_num != 0) {
                        return;
                    }
                    // Some architecture cannot offline cpu0, so assuming it is online
                    is_online = "1";
                }
                is_online = android::base::Trim(is_online);

                (*cpu_usages)[cpu_num].active = user + nice + system;
                (*cpu_usages)[cpu_num].total = user + nice + system + idle;
                (*cpu_usages)[cpu_num].isOnline = (is_online == "1") ? true : false;
            } else {
                LOG(ERROR) << "Unexpected cpu number: " << words[0];
                return;
            }
        }
    }
}

std::map<std::string, std::string> parseThermalPathMap(std::string_view prefix) {
    std::map<std::string, std::string> path_map;
    std::unique_ptr<DIR, int (*)(DIR *)> dir(opendir(kThermalSensorsRoot.data()), closedir);
    if (!dir) {
        return path_map;
    }

    // std::filesystem is not available for vendor yet
    // see discussion: aosp/894015
    while (struct dirent *dp = readdir(dir.get())) {
        if (dp->d_type != DT_DIR) {
            continue;
        }

        if (!android::base::StartsWith(dp->d_name, prefix.data())) {
            continue;
        }

        std::string path = android::base::StringPrintf("%s/%s/%s", kThermalSensorsRoot.data(),
                                                       dp->d_name, kThermalNameFile.data());
        std::string name;
        if (!android::base::ReadFileToString(path, &name)) {
            PLOG(ERROR) << "Failed to read from " << path;
            continue;
        }

        path_map.emplace(
                android::base::Trim(name),
                android::base::StringPrintf("%s/%s", kThermalSensorsRoot.data(), dp->d_name));
    }

    return path_map;
}

}  // namespace
PowerHalService::PowerHalService()
    : power_hal_aidl_exist_(true), power_hal_aidl_(nullptr), power_hal_ext_aidl_(nullptr) {
    connect();
}

bool PowerHalService::connect() {
    std::lock_guard<std::mutex> lock(lock_);
    if (!power_hal_aidl_exist_)
        return false;

    if (power_hal_aidl_ != nullptr)
        return true;

    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder power_binder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    ndk::SpAIBinder ext_power_binder;

    if (power_binder.get() == nullptr) {
        LOG(ERROR) << "Cannot get Power Hal Binder";
        power_hal_aidl_exist_ = false;
        return false;
    }

    power_hal_aidl_ = IPower::fromBinder(power_binder);

    if (power_hal_aidl_ == nullptr) {
        power_hal_aidl_exist_ = false;
        LOG(ERROR) << "Cannot get Power Hal AIDL" << kInstance.c_str();
        return false;
    }

    if (STATUS_OK != AIBinder_getExtension(power_binder.get(), ext_power_binder.getR()) ||
        ext_power_binder.get() == nullptr) {
        LOG(ERROR) << "Cannot get Power Hal Extension Binder";
        power_hal_aidl_exist_ = false;
        return false;
    }

    power_hal_ext_aidl_ = IPowerExt::fromBinder(ext_power_binder);
    if (power_hal_ext_aidl_ == nullptr) {
        LOG(ERROR) << "Cannot get Power Hal Extension AIDL";
        power_hal_aidl_exist_ = false;
    }

    return true;
}

bool PowerHalService::isModeSupported(const std::string &type, const ThrottlingSeverity &t) {
    bool isSupported = false;
    if (!isPowerHalConnected()) {
        return false;
    }
    std::string power_hint = StringPrintf("THERMAL_%s_%s", type.c_str(), toString(t).c_str());
    lock_.lock();
    if (!power_hal_ext_aidl_->isModeSupported(power_hint, &isSupported).isOk()) {
        LOG(ERROR) << "Fail to check supported mode, Hint: " << power_hint;
        power_hal_aidl_exist_ = false;
        power_hal_ext_aidl_ = nullptr;
        power_hal_aidl_ = nullptr;
        lock_.unlock();
        return false;
    }
    lock_.unlock();
    return isSupported;
}

void PowerHalService::setMode(const std::string &type, const ThrottlingSeverity &t,
                              const bool &enable) {
    if (!isPowerHalConnected()) {
        return;
    }

    std::string power_hint = StringPrintf("THERMAL_%s_%s", type.c_str(), toString(t).c_str());
    LOG(INFO) << "Send Hint " << power_hint << " Enable: " << std::boolalpha << enable;
    lock_.lock();
    if (!power_hal_ext_aidl_->setMode(power_hint, enable).isOk()) {
        LOG(ERROR) << "Fail to set mode, Hint: " << power_hint;
        power_hal_aidl_exist_ = false;
        power_hal_ext_aidl_ = nullptr;
        power_hal_aidl_ = nullptr;
        lock_.unlock();
        return;
    }
    lock_.unlock();
}

/*
 * Populate the sensor_name_to_file_map_ map by walking through the file tree,
 * reading the type file and assigning the temp file path to the map.  If we do
 * not succeed, abort.
 */
ThermalHelper::ThermalHelper(const NotificationCallback &cb)
    : thermal_watcher_(new ThermalWatcher(
              std::bind(&ThermalHelper::thermalWatcherCallbackFunc, this, std::placeholders::_1))),
      cb_(cb),
      cooling_device_info_map_(ParseCoolingDevice(
              "/vendor/etc/" +
              android::base::GetProperty(kConfigProperty.data(), kConfigDefaultFileName.data()))),
      sensor_info_map_(ParseSensorInfo(
              "/vendor/etc/" +
              android::base::GetProperty(kConfigProperty.data(), kConfigDefaultFileName.data()))) {
    for (auto const &name_status_pair : sensor_info_map_) {
        sensor_status_map_[name_status_pair.first] = {
                .severity = ThrottlingSeverity::NONE,
                .prev_hot_severity = ThrottlingSeverity::NONE,
                .prev_cold_severity = ThrottlingSeverity::NONE,
                .prev_hint_severity = ThrottlingSeverity::NONE,
                .err_integral = 0.0,
                .prev_err = NAN,
        };

        for (auto const &cdev_request_name :
             name_status_pair.second.throttling_info->cdev_request) {
            if (!cooling_device_info_map_.count(cdev_request_name)) {
                LOG(FATAL) << "Could not find " << cdev_request_name
                           << " in cooling device info map";
            }
            sensor_status_map_[name_status_pair.first].pid_request_map[cdev_request_name] = 0;
            cdev_status_map_[cdev_request_name][name_status_pair.first] = 0;
        }

        for (auto const &limit_info_pair : name_status_pair.second.throttling_info->limit_info) {
            if (!cooling_device_info_map_.count(limit_info_pair.first)) {
                LOG(FATAL) << "Could not find " << limit_info_pair.first
                           << " in cooling device info map";
            }
            sensor_status_map_[name_status_pair.first]
                    .hard_limit_request_map[limit_info_pair.first] = 0;
            cdev_status_map_[limit_info_pair.first][name_status_pair.first] = 0;
        }
    }

    auto tz_map = parseThermalPathMap(kSensorPrefix.data());
    auto cdev_map = parseThermalPathMap(kCoolingDevicePrefix.data());

    is_initialized_ = initializeSensorMap(tz_map) && initializeCoolingDevices(cdev_map);
    if (!is_initialized_) {
        LOG(FATAL) << "ThermalHAL could not be initialized properly.";
    }

    std::set<std::string> monitored_sensors;
    initializeTrip(tz_map, &monitored_sensors);
    thermal_watcher_->registerFilesToWatch(monitored_sensors);

    // Need start watching after status map initialized
    is_initialized_ = thermal_watcher_->startWatchingDeviceFiles();
    if (!is_initialized_) {
        LOG(FATAL) << "ThermalHAL could not start watching thread properly.";
    }

    if (!connectToPowerHal()) {
        LOG(ERROR) << "Fail to connect to Power Hal";
    } else {
        updateSupportedPowerHints();
    }
}

bool ThermalHelper::readCoolingDevice(std::string_view cooling_device,
                                      CoolingDevice_2_0 *out) const {
    // Read the file.  If the file can't be read temp will be empty string.
    std::string data;

    if (!cooling_devices_.readThermalFile(cooling_device, &data)) {
        LOG(ERROR) << "readCoolingDevice: failed to read cooling_device: " << cooling_device;
        return false;
    }

    const CdevInfo &cdev_info = cooling_device_info_map_.at(cooling_device.data());
    const CoolingType &type = cdev_info.type;

    out->type = type;
    out->name = cooling_device.data();
    out->value = std::stoi(data);

    return true;
}

bool ThermalHelper::readTemperature(std::string_view sensor_name, Temperature_1_0 *out) const {
    // Read the file.  If the file can't be read temp will be empty string.
    std::string temp;

    if (!thermal_sensors_.readThermalFile(sensor_name, &temp)) {
        LOG(ERROR) << "readTemperature: sensor not found: " << sensor_name;
        return false;
    }

    if (temp.empty()) {
        LOG(ERROR) << "readTemperature: failed to read sensor: " << sensor_name;
        return false;
    }

    const SensorInfo &sensor_info = sensor_info_map_.at(sensor_name.data());
    TemperatureType_1_0 type =
        (static_cast<int>(sensor_info.type) > static_cast<int>(TemperatureType_1_0::SKIN))
            ? TemperatureType_1_0::UNKNOWN
            : static_cast<TemperatureType_1_0>(sensor_info.type);
    out->type = type;
    out->name = sensor_name.data();
    out->currentValue = std::stof(temp) * sensor_info.multiplier;
    out->throttlingThreshold =
        sensor_info.hot_thresholds[static_cast<size_t>(ThrottlingSeverity::SEVERE)];
    out->shutdownThreshold =
        sensor_info.hot_thresholds[static_cast<size_t>(ThrottlingSeverity::SHUTDOWN)];
    out->vrThrottlingThreshold = sensor_info.vr_threshold;

    return true;
}

bool ThermalHelper::readTemperature(
        std::string_view sensor_name, Temperature_2_0 *out,
        std::pair<ThrottlingSeverity, ThrottlingSeverity> *throtting_status,
        bool is_virtual_sensor) const {
    // Read the file.  If the file can't be read temp will be empty string.
    std::string temp;

    if (!is_virtual_sensor) {
        if (!thermal_sensors_.readThermalFile(sensor_name, &temp)) {
            LOG(ERROR) << "readTemperature: sensor not found: " << sensor_name;
            return false;
        }

        if (temp.empty()) {
            LOG(ERROR) << "readTemperature: failed to read sensor: " << sensor_name;
            return false;
        }
    } else {
        if (!checkVirtualSensor(sensor_name.data(), &temp)) {
            LOG(ERROR) << "readTemperature: failed to read virtual sensor: " << sensor_name;
            return false;
        }
    }

    const auto &sensor_info = sensor_info_map_.at(sensor_name.data());
    out->type = sensor_info.type;
    out->name = sensor_name.data();
    out->value = std::stof(temp) * sensor_info.multiplier;

    std::pair<ThrottlingSeverity, ThrottlingSeverity> status =
        std::make_pair(ThrottlingSeverity::NONE, ThrottlingSeverity::NONE);
    // Only update status if the thermal sensor is being monitored
    if (sensor_info.is_monitor) {
        ThrottlingSeverity prev_hot_severity, prev_cold_severity;
        {
            // reader lock, readTemperature will be called in Binder call and the watcher thread.
            std::shared_lock<std::shared_mutex> _lock(sensor_status_map_mutex_);
            prev_hot_severity = sensor_status_map_.at(sensor_name.data()).prev_hot_severity;
            prev_cold_severity = sensor_status_map_.at(sensor_name.data()).prev_cold_severity;
        }
        status = getSeverityFromThresholds(sensor_info.hot_thresholds, sensor_info.cold_thresholds,
                                           sensor_info.hot_hysteresis, sensor_info.cold_hysteresis,
                                           prev_hot_severity, prev_cold_severity, out->value);
    }
    if (throtting_status) {
        *throtting_status = status;
    }

    out->throttlingStatus = static_cast<size_t>(status.first) > static_cast<size_t>(status.second)
                                ? status.first
                                : status.second;

    return true;
}

bool ThermalHelper::readTemperatureThreshold(std::string_view sensor_name,
                                             TemperatureThreshold *out) const {
    // Read the file.  If the file can't be read temp will be empty string.
    std::string temp;
    std::string path;

    if (!sensor_info_map_.count(sensor_name.data())) {
        LOG(ERROR) << __func__ << ": sensor not found: " << sensor_name;
        return false;
    }

    const auto &sensor_info = sensor_info_map_.at(sensor_name.data());

    out->type = sensor_info.type;
    out->name = sensor_name.data();
    out->hotThrottlingThresholds = sensor_info.hot_thresholds;
    out->coldThrottlingThresholds = sensor_info.cold_thresholds;
    out->vrThrottlingThreshold = sensor_info.vr_threshold;
    return true;
}

// Return the power budget which is computed by PID algorithm
float ThermalHelper::pidPowerCalculator(const Temperature_2_0 &temp, const SensorInfo &sensor_info,
                                        SensorStatus *sensor_status,
                                        std::chrono::milliseconds time_elapsed_ms) {
    float p = 0, i = 0, d = 0;
    size_t target_state = 0;
    float power_budget = std::numeric_limits<float>::max();

    for (const auto &severity : hidl_enum_range<ThrottlingSeverity>()) {
        size_t state = static_cast<size_t>(severity);
        if (sensor_info.throttling_info->throttle_type[state] != PID) {
            continue;
        }
        target_state = state;
        if (severity > sensor_status->severity) {
            target_state = state;
            break;
        }
    }

    LOG(VERBOSE) << "PID target state=" << target_state;
    if (!target_state || (sensor_status->severity == ThrottlingSeverity::NONE)) {
        sensor_status->err_integral = 0;
        sensor_status->prev_err = NAN;
        return power_budget;
    }

    // Compute PID
    float err = sensor_info.hot_thresholds[target_state] - temp.value;
    p = err * (err < 0 ? sensor_info.throttling_info->k_po[target_state]
                       : sensor_info.throttling_info->k_pu[target_state]);
    i = sensor_status->err_integral * sensor_info.throttling_info->k_i[target_state];
    if (err < sensor_info.throttling_info->i_cutoff[target_state]) {
        float i_next = i + err * sensor_info.throttling_info->k_i[target_state];
        if (abs(i_next) < sensor_info.throttling_info->i_max[target_state]) {
            i = i_next;
            sensor_status->err_integral += err;
        }
    }

    if (!std::isnan(sensor_status->prev_err)) {
        d = sensor_info.throttling_info->k_d[target_state] * (err - sensor_status->prev_err) /
            time_elapsed_ms.count();
    }

    sensor_status->prev_err = err;
    // Calculate power budget
    power_budget = sensor_info.throttling_info->s_power[target_state] + p + i + d;
    if (power_budget < sensor_info.throttling_info->min_alloc_power[target_state]) {
        power_budget = sensor_info.throttling_info->min_alloc_power[target_state];
    }
    if (power_budget > sensor_info.throttling_info->max_alloc_power[target_state]) {
        power_budget = sensor_info.throttling_info->max_alloc_power[target_state];
    }

    LOG(VERBOSE) << " power_budget=" << power_budget << " err=" << err
                 << " err_integral=" << sensor_status->err_integral
                 << " s_power=" << sensor_info.throttling_info->s_power[target_state]
                 << " time_elpased_ms=" << time_elapsed_ms.count() << " p=" << p << " i=" << i
                 << " d=" << d;

    return power_budget;
}

bool ThermalHelper::requestCdevByPower(std::string_view sensor_name, SensorStatus *sensor_status,
                                       const SensorInfo &sensor_info, float total_power_budget) {
    float total_weight = 0, cdev_power_budget;
    size_t i, j;

    for (i = 0; i < sensor_info.throttling_info->cdev_request.size(); ++i) {
        total_weight += sensor_info.throttling_info->cdev_weight[i];
    }

    if (!total_weight) {
        LOG(ERROR) << "Sensor: " << sensor_name.data() << " total weight value is zero";
        return false;
    }

    // Map cdev state by power
    for (i = 0; i < sensor_info.throttling_info->cdev_request.size(); ++i) {
        if (sensor_info.throttling_info->cdev_request[i] != "") {
            cdev_power_budget = total_power_budget *
                                ((sensor_info.throttling_info->cdev_weight[i]) / total_weight);

            const CdevInfo &cdev_info_pair =
                    cooling_device_info_map_.at(sensor_info.throttling_info->cdev_request[i]);
            for (j = 0; j < cdev_info_pair.power2state.size() - 1; ++j) {
                if (cdev_power_budget > cdev_info_pair.power2state[j]) {
                    break;
                }
            }
            sensor_status->pid_request_map.at(sensor_info.throttling_info->cdev_request[i]) =
                    static_cast<int>(j);
            LOG(VERBOSE) << "Power allocator: Sensor " << sensor_name.data() << " allocate "
                         << sensor_info.throttling_info->cdev_request[i] << " " << cdev_power_budget
                         << "mW, update state to " << j;
        }
    }
    return true;
}

void ThermalHelper::requestCdevBySeverity(std::string_view sensor_name, SensorStatus *sensor_status,
                                          const SensorInfo &sensor_info) {
    size_t target_state = 0;
    for (size_t i = static_cast<size_t>(sensor_status->severity);
         i > static_cast<size_t>(ThrottlingSeverity::NONE); --i) {
        if (sensor_info.throttling_info->throttle_type[i] == LIMIT) {
            target_state = i;
            break;
        }
    }
    LOG(VERBOSE) << "Hard Limit target state=" << target_state;

    for (auto const &limit_info_pair : sensor_info.throttling_info->limit_info) {
        sensor_status->hard_limit_request_map.at(limit_info_pair.first) =
                limit_info_pair.second[target_state];
        LOG(VERBOSE) << "Hard Limit: Sensor " << sensor_name.data() << " update cdev "
                     << limit_info_pair.first << " to "
                     << sensor_status->hard_limit_request_map.at(limit_info_pair.first);
    }
}

void ThermalHelper::updateCoolingDevices(const std::vector<std::string> &updated_cdev) {
    int max_state;

    for (const auto &target_cdev : updated_cdev) {
        max_state = 0;
        const CdevRequestStatus &cdev_status = cdev_status_map_.at(target_cdev);
        for (auto &sensor_request_pair : cdev_status) {
            if (sensor_request_pair.second > max_state) {
                max_state = sensor_request_pair.second;
            }
        }
        if (cooling_devices_.writeCdevFile(target_cdev, std::to_string(max_state))) {
            LOG(VERBOSE) << "Successfully update cdev " << target_cdev << " sysfs to " << max_state;
        }
    }
}

std::pair<ThrottlingSeverity, ThrottlingSeverity> ThermalHelper::getSeverityFromThresholds(
    const ThrottlingArray &hot_thresholds, const ThrottlingArray &cold_thresholds,
    const ThrottlingArray &hot_hysteresis, const ThrottlingArray &cold_hysteresis,
    ThrottlingSeverity prev_hot_severity, ThrottlingSeverity prev_cold_severity,
    float value) const {
    ThrottlingSeverity ret_hot = ThrottlingSeverity::NONE;
    ThrottlingSeverity ret_hot_hysteresis = ThrottlingSeverity::NONE;
    ThrottlingSeverity ret_cold = ThrottlingSeverity::NONE;
    ThrottlingSeverity ret_cold_hysteresis = ThrottlingSeverity::NONE;

    // Here we want to control the iteration from high to low, and hidl_enum_range doesn't support
    // a reverse iterator yet.
    for (size_t i = static_cast<size_t>(ThrottlingSeverity::SHUTDOWN);
         i > static_cast<size_t>(ThrottlingSeverity::NONE); --i) {
        if (!std::isnan(hot_thresholds[i]) && hot_thresholds[i] <= value &&
            ret_hot == ThrottlingSeverity::NONE) {
            ret_hot = static_cast<ThrottlingSeverity>(i);
        }
        if (!std::isnan(hot_thresholds[i]) && (hot_thresholds[i] - hot_hysteresis[i]) < value &&
            ret_hot_hysteresis == ThrottlingSeverity::NONE) {
            ret_hot_hysteresis = static_cast<ThrottlingSeverity>(i);
        }
        if (!std::isnan(cold_thresholds[i]) && cold_thresholds[i] >= value &&
            ret_cold == ThrottlingSeverity::NONE) {
            ret_cold = static_cast<ThrottlingSeverity>(i);
        }
        if (!std::isnan(cold_thresholds[i]) && (cold_thresholds[i] + cold_hysteresis[i]) > value &&
            ret_cold_hysteresis == ThrottlingSeverity::NONE) {
            ret_cold_hysteresis = static_cast<ThrottlingSeverity>(i);
        }
    }
    if (static_cast<size_t>(ret_hot) < static_cast<size_t>(prev_hot_severity)) {
        ret_hot = ret_hot_hysteresis;
    }
    if (static_cast<size_t>(ret_cold) < static_cast<size_t>(prev_cold_severity)) {
        ret_cold = ret_cold_hysteresis;
    }

    return std::make_pair(ret_hot, ret_cold);
}

bool ThermalHelper::initializeSensorMap(const std::map<std::string, std::string> &path_map) {
    for (const auto &sensor_info_pair : sensor_info_map_) {
        std::string_view sensor_name = sensor_info_pair.first;
        if (sensor_info_pair.second.is_virtual_sensor) {
            sensor_name = sensor_info_pair.second.trigger_sensor;
        }
        if (!path_map.count(sensor_name.data())) {
            LOG(ERROR) << "Could not find " << sensor_name << " in sysfs";
            continue;
        }
        std::string path = android::base::StringPrintf(
                "%s/%s", path_map.at(sensor_name.data()).c_str(), kSensorTempSuffix.data());
        if (sensor_info_pair.second.is_virtual_sensor) {
            sensor_name = sensor_info_pair.first;
        }
        if (!thermal_sensors_.addThermalFile(sensor_name, path)) {
            LOG(ERROR) << "Could not add " << sensor_name << "to sensors map";
        }
    }
    if (sensor_info_map_.size() == thermal_sensors_.getNumThermalFiles()) {
        return true;
    }
    return false;
}

bool ThermalHelper::initializeCoolingDevices(const std::map<std::string, std::string> &path_map) {
    for (const auto &cooling_device_info_pair : cooling_device_info_map_) {
        std::string_view cooling_device_name = cooling_device_info_pair.first;
        if (!path_map.count(cooling_device_name.data())) {
            LOG(ERROR) << "Could not find " << cooling_device_name << " in sysfs";
            continue;
        }
        std::string path = android::base::StringPrintf(
                "%s/%s", path_map.at(cooling_device_name.data()).c_str(),
                kCoolingDeviceCurStateSuffix.data());
        if (!cooling_devices_.addThermalFile(cooling_device_name, path)) {
            LOG(ERROR) << "Could not add " << cooling_device_name << "to cooling device map";
            continue;
        }
    }

    if (cooling_device_info_map_.size() == cooling_devices_.getNumThermalFiles()) {
        return true;
    }
    return false;
}

void ThermalHelper::setMinTimeout(SensorInfo *sensor_info) {
    sensor_info->polling_delay = kMinPollIntervalMs;
    sensor_info->passive_delay = kMinPollIntervalMs;
}

void ThermalHelper::initializeTrip(const std::map<std::string, std::string> &path_map,
                                   std::set<std::string> *monitored_sensors) {
    for (auto &sensor_info : sensor_info_map_) {
        if (!sensor_info.second.is_monitor || sensor_info.second.is_virtual_sensor) {
            continue;
        }

        bool support_uevent = false;
        std::string_view sensor_name = sensor_info.first;
        std::string_view tz_path = path_map.at(sensor_name.data());
        std::string tz_policy;
        std::string path =
                android::base::StringPrintf("%s/%s", (tz_path.data()), kSensorPolicyFile.data());
        // Check if thermal zone support uevent notify
        if (!android::base::ReadFileToString(path, &tz_policy)) {
            LOG(ERROR) << sensor_name << " could not open tz policy file:" << path;
        } else {
            tz_policy = android::base::Trim(tz_policy);
            if (tz_policy != kUserSpaceSuffix) {
                LOG(ERROR) << sensor_name << " does not support uevent notify";
            } else {
                support_uevent = true;
            }
        }

        if (support_uevent) {
            // Update thermal zone trip point
            for (size_t i = 0; i < kThrottlingSeverityCount; ++i) {
                if (!std::isnan(sensor_info.second.hot_thresholds[i]) &&
                    !std::isnan(sensor_info.second.hot_hysteresis[i])) {
                    // Update trip_point_0_temp threshold
                    std::string threshold = std::to_string(static_cast<int>(
                            sensor_info.second.hot_thresholds[i] / sensor_info.second.multiplier));
                    path = android::base::StringPrintf("%s/%s", (tz_path.data()),
                                                       kSensorTripPointTempZeroFile.data());
                    if (!android::base::WriteStringToFile(threshold, path)) {
                        LOG(ERROR) << "fail to update " << sensor_name << " trip point: " << path
                                   << " to " << threshold;
                        support_uevent = false;
                        break;
                    }
                    // Update trip_point_0_hyst threshold
                    threshold = std::to_string(static_cast<int>(
                            sensor_info.second.hot_hysteresis[i] / sensor_info.second.multiplier));
                    path = android::base::StringPrintf("%s/%s", (tz_path.data()),
                                                       kSensorTripPointHystZeroFile.data());
                    if (!android::base::WriteStringToFile(threshold, path)) {
                        LOG(ERROR) << "fail to update " << sensor_name << "trip hyst" << threshold
                                   << path;
                        support_uevent = false;
                        break;
                    }
                    break;
                } else if (i == kThrottlingSeverityCount - 1) {
                    LOG(ERROR) << sensor_name << ":all thresholds are NAN";
                    support_uevent = false;
                    break;
                }
            }
        }
        if (support_uevent) {
            monitored_sensors->insert(sensor_info.first);
        } else {
            LOG(INFO) << "config Sensor: " << sensor_info.first
                      << " to default polling interval: " << kMinPollIntervalMs.count();
            setMinTimeout(&sensor_info.second);
        }
    }
}
bool ThermalHelper::fillTemperatures(hidl_vec<Temperature_1_0> *temperatures) const {
    temperatures->resize(sensor_info_map_.size());
    int current_index = 0;
    for (const auto &name_info_pair : sensor_info_map_) {
        Temperature_1_0 temp;

        if (readTemperature(name_info_pair.first, &temp)) {
            (*temperatures)[current_index] = temp;
        } else {
            LOG(ERROR) << __func__
                       << ": error reading temperature for sensor: " << name_info_pair.first;
            return false;
        }
        ++current_index;
    }
    return current_index > 0;
}

bool ThermalHelper::fillCurrentTemperatures(bool filterType, TemperatureType_2_0 type,
                                            hidl_vec<Temperature_2_0> *temperatures) const {
    std::vector<Temperature_2_0> ret;
    for (const auto &name_info_pair : sensor_info_map_) {
        Temperature_2_0 temp;
        if (filterType && name_info_pair.second.type != type) {
            continue;
        }
        if (readTemperature(name_info_pair.first, &temp)) {
            ret.emplace_back(std::move(temp));
        } else {
            LOG(ERROR) << __func__
                       << ": error reading temperature for sensor: " << name_info_pair.first;
            return false;
        }
    }
    *temperatures = ret;
    return ret.size() > 0;
}

bool ThermalHelper::fillTemperatureThresholds(bool filterType, TemperatureType_2_0 type,
                                              hidl_vec<TemperatureThreshold> *thresholds) const {
    std::vector<TemperatureThreshold> ret;
    for (const auto &name_info_pair : sensor_info_map_) {
        TemperatureThreshold temp;
        if (filterType && name_info_pair.second.type != type) {
            continue;
        }
        if (readTemperatureThreshold(name_info_pair.first, &temp)) {
            ret.emplace_back(std::move(temp));
        } else {
            LOG(ERROR) << __func__ << ": error reading temperature threshold for sensor: "
                       << name_info_pair.first;
            return false;
        }
    }
    *thresholds = ret;
    return ret.size() > 0;
}

bool ThermalHelper::fillCurrentCoolingDevices(bool filterType, CoolingType type,
                                              hidl_vec<CoolingDevice_2_0> *cooling_devices) const {
    std::vector<CoolingDevice_2_0> ret;
    for (const auto &name_info_pair : cooling_device_info_map_) {
        CoolingDevice_2_0 value;
        if (filterType && name_info_pair.second.type != type) {
            continue;
        }
        if (readCoolingDevice(name_info_pair.first, &value)) {
            ret.emplace_back(std::move(value));
        } else {
            LOG(ERROR) << __func__ << ": error reading cooling device: " << name_info_pair.first;
            return false;
        }
    }
    *cooling_devices = ret;
    return ret.size() > 0;
}

bool ThermalHelper::fillCpuUsages(hidl_vec<CpuUsage> *cpu_usages) const {
    cpu_usages->resize(kMaxCpus);
    for (int i = 0; i < kMaxCpus; i++) {
        (*cpu_usages)[i].name = StringPrintf("cpu%d", i);
        (*cpu_usages)[i].active = 0;
        (*cpu_usages)[i].total = 0;
        (*cpu_usages)[i].isOnline = false;
    }
    parseCpuUsagesFileAndAssignUsages(cpu_usages);
    return true;
}

bool ThermalHelper::checkVirtualSensor(std::string_view sensor_name, std::string *temp) const {
    double mTemp = 0;
    for (const auto &sensor_info_pair : sensor_info_map_) {
        int idx = 0;
        if (sensor_info_pair.first != sensor_name.data()) {
            continue;
        }
        for (int i = 0; i < static_cast<int>(kCombinationCount); i++) {
            if (sensor_info_pair.second.linked_sensors[i].compare("NAN") == 0) {
                continue;
            }
            if (sensor_info_pair.second.linked_sensors[i].size() <= 0) {
                continue;
            }
            std::string data;
            idx += 1;
            if (!thermal_sensors_.readThermalFile(sensor_info_pair.second.linked_sensors[i],
                                                  &data)) {
                continue;
            }
            data = ::android::base::Trim(data);
            float sensor_reading = std::stof(data);
            if (std::isnan(sensor_info_pair.second.coefficients[i])) {
                continue;
            }
            float coefficient = sensor_info_pair.second.coefficients[i];
            switch (sensor_info_pair.second.formula) {
                case FormulaOption::COUNT_THRESHOLD:
                    if ((coefficient < 0 && sensor_reading < -coefficient) ||
                        (coefficient >= 0 && sensor_reading >= coefficient))
                        mTemp += 1;
                    break;
                case FormulaOption::WEIGHTED_AVG:
                    mTemp += sensor_reading * coefficient;
                    break;
                case FormulaOption::MAXIMUM:
                    if (i == 0)
                        mTemp = std::numeric_limits<float>::lowest();
                    if (sensor_reading * coefficient > mTemp)
                        mTemp = sensor_reading * coefficient;
                    break;
                case FormulaOption::MINIMUM:
                    if (i == 0)
                        mTemp = std::numeric_limits<float>::max();
                    if (sensor_reading * coefficient < mTemp)
                        mTemp = sensor_reading * coefficient;
                    break;
                default:
                    break;
            }
        }
        *temp = std::to_string(mTemp);
        return true;
    }
    return false;
}

// This is called in the different thread context and will update sensor_status
// uevent_sensors is the set of sensors which trigger uevent from thermal core driver.
std::chrono::milliseconds ThermalHelper::thermalWatcherCallbackFunc(
        const std::set<std::string> &uevent_sensors) {
    std::vector<Temperature_2_0> temps;
    std::vector<std::string> cooling_devices_to_update;
    boot_clock::time_point now = boot_clock::now();
    auto min_sleep_ms = std::chrono::milliseconds(std::numeric_limits<int>::max());

    for (auto &name_status_pair : sensor_status_map_) {
        Temperature_2_0 temp;
        TemperatureThreshold threshold;
        bool is_virtual_sensor = false;
        SensorStatus &sensor_status = name_status_pair.second;
        const SensorInfo &sensor_info = sensor_info_map_.at(name_status_pair.first);

        // Only handle the sensors in allow list
        if (!sensor_info.is_monitor) {
            continue;
        }

        is_virtual_sensor = sensor_info.is_virtual_sensor;

        std::string uevent_sensor_name = name_status_pair.first;
        if (is_virtual_sensor) {
            const SensorInfo &sensor_info = sensor_info_map_.at(name_status_pair.first);
            uevent_sensor_name = sensor_info.trigger_sensor;
        }

        // Check if the sensor need to be updated
        auto time_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - sensor_status.last_update_time);
        auto sleep_ms = (sensor_status.severity != ThrottlingSeverity::NONE)
                                ? sensor_info.passive_delay
                                : sensor_info.polling_delay;
        LOG(VERBOSE) << "sensor " << name_status_pair.first
                     << ": time_elpased=" << time_elapsed_ms.count()
                     << ", sleep_ms=" << sleep_ms.count();
        if ((time_elapsed_ms < sleep_ms) &&
            (!uevent_sensors.size() ||
             uevent_sensors.find(uevent_sensor_name) == uevent_sensors.end())) {
            auto timeout_remaining = sleep_ms - time_elapsed_ms;
            if (min_sleep_ms > timeout_remaining) {
                min_sleep_ms = timeout_remaining;
            }
            LOG(VERBOSE) << "sensor " << name_status_pair.first
                         << ": timeout_remaining=" << timeout_remaining.count();
            continue;
        }

        std::pair<ThrottlingSeverity, ThrottlingSeverity> throtting_status;
        if (!readTemperature(name_status_pair.first, &temp, &throtting_status, is_virtual_sensor)) {
            LOG(ERROR) << __func__
                       << ": error reading temperature for sensor: " << name_status_pair.first;
            continue;
        }
        if (!readTemperatureThreshold(name_status_pair.first, &threshold)) {
            LOG(ERROR) << __func__ << ": error reading temperature threshold for sensor: "
                       << name_status_pair.first;
            continue;
        }

        {
            // writer lock
            std::unique_lock<std::shared_mutex> _lock(sensor_status_map_mutex_);
            if (throtting_status.first != sensor_status.prev_hot_severity) {
                sensor_status.prev_hot_severity = throtting_status.first;
            }
            if (throtting_status.second != sensor_status.prev_cold_severity) {
                sensor_status.prev_cold_severity = throtting_status.second;
            }
            if (temp.throttlingStatus != sensor_status.severity) {
                temps.push_back(temp);
                sensor_status.severity = temp.throttlingStatus;
                sleep_ms = (sensor_status.severity != ThrottlingSeverity::NONE)
                                   ? sensor_info.passive_delay
                                   : sensor_info.polling_delay;
            }
        }

        if (sensor_status.severity != ThrottlingSeverity::NONE) {
            LOG(INFO) << temp.name << ": " << temp.value << " degC";
        }

        // Start PID computation
        if (sensor_status.pid_request_map.size()) {
            float power_budget =
                    pidPowerCalculator(temp, sensor_info, &sensor_status, time_elapsed_ms);
            if (!requestCdevByPower(name_status_pair.first, &sensor_status, sensor_info,
                                    power_budget)) {
                LOG(ERROR) << "Sensor " << temp.name << " PID request cdev failed";
            }
        }

        if (sensor_status.hard_limit_request_map.size()) {
            // Start hard limit computation
            requestCdevBySeverity(name_status_pair.first, &sensor_status, sensor_info);
        }
        if (sensor_status.pid_request_map.size() || sensor_status.hard_limit_request_map.size()) {
            // Aggregate cooling device request
            for (auto &cdev_request_pair : cdev_status_map_) {
                int pid_max = 0;
                int limit_max = 0;
                if (sensor_status.pid_request_map.size()) {
                    pid_max = sensor_status.pid_request_map.at(cdev_request_pair.first);
                }
                if (sensor_status.hard_limit_request_map.size()) {
                    limit_max = sensor_status.hard_limit_request_map.at(cdev_request_pair.first);
                }

                int request_state = fmax(pid_max, limit_max);
                LOG(VERBOSE) << "Sensor " << name_status_pair.first << ": "
                             << cdev_request_pair.first << " aggregation result is "
                             << request_state;

                if (cdev_request_pair.second.at(name_status_pair.first) != request_state) {
                    cdev_request_pair.second.at(name_status_pair.first) = request_state;
                    cooling_devices_to_update.emplace_back(cdev_request_pair.first);
                }
            }
        }

        if (min_sleep_ms > sleep_ms) {
            min_sleep_ms = sleep_ms;
        }
        LOG(VERBOSE) << "Sensor " << name_status_pair.first << ": sleep_ms=" << sleep_ms.count()
                     << ", min_sleep_ms voting result=" << min_sleep_ms.count();
        sensor_status.last_update_time = now;
    }

    if (!cooling_devices_to_update.empty()) {
        updateCoolingDevices(cooling_devices_to_update);
    }

    if (!temps.empty()) {
        for (const auto &t : temps) {
            if (sensor_info_map_.at(t.name).send_cb && cb_) {
                cb_(t);
            }

            if (sensor_info_map_.at(t.name).send_powerhint && isAidlPowerHalExist()) {
                sendPowerExtHint(t);
            }
        }
    }

    return min_sleep_ms < kMinPollIntervalMs ? kMinPollIntervalMs : min_sleep_ms;
}

bool ThermalHelper::connectToPowerHal() {
    return power_hal_service_.connect();
}

void ThermalHelper::updateSupportedPowerHints() {
    for (auto const &name_status_pair : sensor_info_map_) {
        if (!(name_status_pair.second.send_powerhint)) {
            continue;
        }
        ThrottlingSeverity current_severity = ThrottlingSeverity::NONE;
        for (const auto &severity : hidl_enum_range<ThrottlingSeverity>()) {
            LOG(ERROR) << "sensor: " << name_status_pair.first
                       << " current_severity :" << toString(current_severity) << " severity "
                       << toString(severity);
            if (severity == ThrottlingSeverity::NONE) {
                supported_powerhint_map_[name_status_pair.first][ThrottlingSeverity::NONE] =
                        ThrottlingSeverity::NONE;
                continue;
            }

            bool isSupported = false;
            ndk::ScopedAStatus isSupportedResult;

            if (power_hal_service_.isPowerHalExtConnected()) {
                isSupported = power_hal_service_.isModeSupported(name_status_pair.first, severity);
            }
            if (isSupported)
                current_severity = severity;
            supported_powerhint_map_[name_status_pair.first][severity] = current_severity;
        }
    }
}

void ThermalHelper::sendPowerExtHint(const Temperature_2_0 &t) {
    std::lock_guard<std::shared_mutex> lock(sensor_status_map_mutex_);

    ThrottlingSeverity prev_hint_severity;
    prev_hint_severity = sensor_status_map_.at(t.name).prev_hint_severity;
    ThrottlingSeverity current_hint_severity = supported_powerhint_map_[t.name][t.throttlingStatus];

    if (prev_hint_severity == current_hint_severity)
        return;

    if (prev_hint_severity != ThrottlingSeverity::NONE) {
        power_hal_service_.setMode(t.name, prev_hint_severity, false);
    }

    if (current_hint_severity != ThrottlingSeverity::NONE) {
        power_hal_service_.setMode(t.name, current_hint_severity, true);
    }

    sensor_status_map_[t.name].prev_hint_severity = current_hint_severity;
}
}  // namespace implementation
}  // namespace V2_0
}  // namespace thermal
}  // namespace hardware
}  // namespace android
