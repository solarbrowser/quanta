/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../../include/platform/NativeAPI.h"
#include <thread>

namespace Quanta {

// Static member for device info
DeviceInfo NativeAPI::device_info_;

DeviceInfo NativeAPI::get_device_info() {
    if (!device_info_.platform_name.empty()) {
        return device_info_;
    }

    switch (current_platform_) {
        case Platform::WINDOWS:
            device_info_.platform_name = "Windows";
            device_info_.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = std::thread::hardware_concurrency();
            device_info_.supported_capabilities =
                static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::BATTERY_INFO) |
                static_cast<uint32_t>(DeviceCapability::CLIPBOARD) |
                static_cast<uint32_t>(DeviceCapability::FILE_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::GAMEPAD) |
                static_cast<uint32_t>(DeviceCapability::SCREEN_INFO) |
                static_cast<uint32_t>(DeviceCapability::SPEECH_SYNTHESIS) |
                static_cast<uint32_t>(DeviceCapability::NETWORK_INFO);
            break;

        case Platform::ANDROID:
            device_info_.platform_name = "Android";
            device_info_.user_agent = "Mozilla/5.0 (Linux; Android 10) Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = std::thread::hardware_concurrency();
            device_info_.supported_capabilities =
                static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::BATTERY_INFO) |
                static_cast<uint32_t>(DeviceCapability::VIBRATION) |
                static_cast<uint32_t>(DeviceCapability::GEOLOCATION) |
                static_cast<uint32_t>(DeviceCapability::CAMERA) |
                static_cast<uint32_t>(DeviceCapability::MICROPHONE) |
                static_cast<uint32_t>(DeviceCapability::ACCELEROMETER) |
                static_cast<uint32_t>(DeviceCapability::GYROSCOPE) |
                static_cast<uint32_t>(DeviceCapability::NETWORK_INFO);
            break;

        case Platform::IOS:
            device_info_.platform_name = "iOS";
            device_info_.user_agent = "Mozilla/5.0 (iPhone; CPU iPhone OS 15_0 like Mac OS X) Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = std::thread::hardware_concurrency();
            device_info_.supported_capabilities =
                static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::BATTERY_INFO) |
                static_cast<uint32_t>(DeviceCapability::VIBRATION) |
                static_cast<uint32_t>(DeviceCapability::GEOLOCATION) |
                static_cast<uint32_t>(DeviceCapability::CAMERA) |
                static_cast<uint32_t>(DeviceCapability::MICROPHONE) |
                static_cast<uint32_t>(DeviceCapability::ACCELEROMETER) |
                static_cast<uint32_t>(DeviceCapability::GYROSCOPE) |
                static_cast<uint32_t>(DeviceCapability::NETWORK_INFO);
            break;

        case Platform::MACOS:
            device_info_.platform_name = "macOS";
            device_info_.user_agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = std::thread::hardware_concurrency();
            device_info_.supported_capabilities =
                static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::BATTERY_INFO) |
                static_cast<uint32_t>(DeviceCapability::CLIPBOARD) |
                static_cast<uint32_t>(DeviceCapability::FILE_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::CAMERA) |
                static_cast<uint32_t>(DeviceCapability::MICROPHONE) |
                static_cast<uint32_t>(DeviceCapability::SCREEN_INFO) |
                static_cast<uint32_t>(DeviceCapability::SPEECH_SYNTHESIS) |
                static_cast<uint32_t>(DeviceCapability::NETWORK_INFO);
            break;

        case Platform::LINUX:
            device_info_.platform_name = "Linux";
            device_info_.user_agent = "Mozilla/5.0 (X11; Linux x86_64) Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = std::thread::hardware_concurrency();
            device_info_.supported_capabilities =
                static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::CLIPBOARD) |
                static_cast<uint32_t>(DeviceCapability::FILE_SYSTEM) |
                static_cast<uint32_t>(DeviceCapability::SCREEN_INFO) |
                static_cast<uint32_t>(DeviceCapability::NETWORK_INFO);
            break;

        default:
            device_info_.platform_name = "Unknown";
            device_info_.user_agent = "Quanta/1.0";
            device_info_.language = "en-US";
            device_info_.languages = {"en-US", "en"};
            device_info_.hardware_concurrency = 1;
            device_info_.supported_capabilities = 0;
            break;
    }

    device_info_.online = true;
    return device_info_;
}

uint32_t NativeAPI::get_device_capabilities() {
    return get_device_info().supported_capabilities;
}

} // namespace Quanta