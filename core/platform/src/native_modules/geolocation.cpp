/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../../include/platform/NativeAPI.h"
#include <map>
#include <functional>

namespace Quanta {

// Static members for geolocation
std::map<int, std::function<void(const GeolocationInfo&)>> NativeAPI::geolocation_watchers_;
int NativeAPI::next_watch_id_ = 1;

GeolocationInfo NativeAPI::get_current_position(bool high_accuracy) {
    (void)high_accuracy; // Suppress unused parameter warning

    GeolocationInfo info;
    info.supported = false;
    info.latitude = 0.0;
    info.longitude = 0.0;
    info.accuracy = 0.0;
    info.altitude = 0.0;
    info.altitude_accuracy = 0.0;
    info.heading = 0.0;
    info.speed = 0.0;
    info.timestamp = 0;

    // Platform-specific geolocation implementation would go here
    switch (current_platform_) {
        case Platform::ANDROID:
        case Platform::IOS:
            // Mobile platforms typically have GPS support
            info.supported = true;
            // Simulate mock location for demo
            info.latitude = 41.0082; // Istanbul coordinates
            info.longitude = 28.9784;
            info.accuracy = 10.0;
            break;

        case Platform::WINDOWS:
        case Platform::MACOS:
        case Platform::LINUX:
            // Desktop platforms may have limited geolocation
            info.supported = false; // Would need platform-specific APIs
            break;

        default:
            info.supported = false;
            break;
    }

    return info;
}

int NativeAPI::watch_position(std::function<void(const GeolocationInfo&)> success_callback,
                             std::function<void(const std::string&)> error_callback,
                             bool high_accuracy) {
    (void)error_callback; // Suppress unused parameter warning

    if (!success_callback) {
        return -1;
    }

    // Generate watch ID and store callback
    int watch_id = next_watch_id_++;
    geolocation_watchers_[watch_id] = success_callback;

    // In a real implementation, this would start periodic position updates
    // For now, we'll just call the callback once with current position
    GeolocationInfo info = get_current_position(high_accuracy);

    if (info.supported) {
        success_callback(info);
    }

    return watch_id;
}

bool NativeAPI::clear_watch_position(int watch_id) {
    auto it = geolocation_watchers_.find(watch_id);
    if (it != geolocation_watchers_.end()) {
        geolocation_watchers_.erase(it);
        return true;
    }
    return false;
}

} // namespace Quanta