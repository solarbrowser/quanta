/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../../include/platform/NativeAPI.h"
#include <iostream>

#if defined(_WIN32) && !defined(__MINGW32__)
#include <windows.h>
#include <versionhelpers.h>
#endif

namespace Quanta {

// Static members for platform detection
Platform NativeAPI::current_platform_ = Platform::UNKNOWN;
bool NativeAPI::initialized_ = false;

Platform NativeAPI::detect_platform() {
    if (current_platform_ != Platform::UNKNOWN) {
        return current_platform_;
    }

#ifdef _WIN32
    current_platform_ = Platform::WINDOWS;
#elif defined(__ANDROID__)
    current_platform_ = Platform::ANDROID;
#elif defined(__APPLE__)
    #ifdef TARGET_OS_IOS
    current_platform_ = Platform::IOS;
    #else
    current_platform_ = Platform::MACOS;
    #endif
#elif defined(__linux__)
    current_platform_ = Platform::LINUX;
#else
    current_platform_ = Platform::UNKNOWN;
#endif

    return current_platform_;
}

bool NativeAPI::initialize_platform_apis() {
    if (initialized_) {
        return true;
    }

    detect_platform();
    initialized_ = true;

    std::cout << "Platform APIs initialized for platform: " << static_cast<int>(current_platform_) << std::endl;
    return true;
}

void NativeAPI::shutdown_platform_apis() {
    initialized_ = false;
    std::cout << "Platform APIs shutdown" << std::endl;
}

} // namespace Quanta