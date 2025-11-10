/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/platform/NativeAPI.h"
#include <string>

namespace Quanta {

/**
 * Platform Detection - Platform and device detection utilities
 * EXTRACTED FROM NativeAPI.cpp - Platform detection functionality
 */
class PlatformDetection {
public:
    // Platform detection
    static Platform detect_platform();
    static std::string get_platform_name(Platform platform = Platform::UNKNOWN);
    static bool is_windows();
    static bool is_linux();
    static bool is_macos();
    static bool is_ios();
    static bool is_android();

    // Device information
    static DeviceInfo detect_device_info();
    static std::string get_cpu_architecture();
    static int get_cpu_core_count();
    static size_t get_total_memory();
    static std::string get_device_model();
    static std::string get_os_version();

    // Feature detection
    static bool supports_threads();
    static bool supports_network();
    static bool supports_filesystem();
    static bool supports_geolocation();
    static bool supports_gamepad();
    static bool supports_notifications();

    // Initialization
    static bool initialize_platform_detection();
    static void cleanup_platform_detection();

private:
    static Platform current_platform_;
    static DeviceInfo device_info_;
    static bool initialized_;

    // Platform-specific detection
    static Platform detect_windows_version();
    static Platform detect_linux_distribution();
    static Platform detect_apple_platform();
};

} // namespace Quanta