/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/platform/APIRouter.h"
#include "quanta/core/apis/WebAPI.h"
#include <iostream>

namespace Quanta {

APIRouter::RoutePreference APIRouter::default_preference_ = APIRouter::RoutePreference::PREFER_NATIVE;
std::map<APIRouter::APICategory, APIRouter::RoutePreference> APIRouter::category_preferences_;
bool APIRouter::initialized_ = false;

void APIRouter::initialize(RoutePreference default_preference) {
    if (initialized_) return;
    
    default_preference_ = default_preference;
    
    if (default_preference != RoutePreference::SIMULATED_ONLY) {
        NativeAPI::initialize_platform_apis();
    }
    
    initialized_ = true;
    
    std::cout << " APIRouter initialized with preference: " << static_cast<int>(default_preference) << std::endl;
}

void APIRouter::set_preference(APICategory category, RoutePreference preference) {
    category_preferences_[category] = preference;
}

APIRouter::RoutePreference APIRouter::get_preference(APICategory category) {
    auto it = category_preferences_.find(category);
    return (it != category_preferences_.end()) ? it->second : default_preference_;
}

bool APIRouter::is_native_available(APICategory category) {
    if (!initialized_) return false;
    
    uint32_t capabilities = NativeAPI::get_device_capabilities();
    
    switch (category) {
        case APICategory::BATTERY:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::BATTERY_INFO)) != 0;
        case APICategory::VIBRATION:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::VIBRATION)) != 0;
        case APICategory::NOTIFICATIONS:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::NOTIFICATION_SYSTEM)) != 0;
        case APICategory::GEOLOCATION:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::GEOLOCATION)) != 0;
        case APICategory::SCREEN:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::SCREEN_INFO)) != 0;
        case APICategory::CLIPBOARD:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::CLIPBOARD)) != 0;
        case APICategory::SPEECH_SYNTHESIS:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::SPEECH_SYNTHESIS)) != 0;
        case APICategory::SPEECH_RECOGNITION:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::SPEECH_RECOGNITION)) != 0;
        case APICategory::GAMEPAD:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::GAMEPAD)) != 0;
        case APICategory::NETWORK:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::NETWORK_INFO)) != 0;
        case APICategory::MEDIA_DEVICES:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::CAMERA)) != 0 ||
                   (capabilities & static_cast<uint32_t>(DeviceCapability::MICROPHONE)) != 0;
        case APICategory::SENSORS:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::SENSORS)) != 0;
        case APICategory::FILE_SYSTEM:
            return (capabilities & static_cast<uint32_t>(DeviceCapability::FILE_SYSTEM)) != 0;
        default:
            return true;
    }
}

bool APIRouter::should_use_native(APICategory category) {
    if (!initialized_) {
        initialize();
    }
    
    RoutePreference pref = get_preference(category);
    bool native_available = is_native_available(category);
    
    switch (pref) {
        case RoutePreference::PREFER_NATIVE:
            return native_available;
        case RoutePreference::PREFER_SIMULATED:
            return false;
        case RoutePreference::NATIVE_ONLY:
            return native_available;
        case RoutePreference::SIMULATED_ONLY:
            return false;
        default:
            return native_available;
    }
}

Value APIRouter::get_battery_charging(Context& ctx, const std::vector<Value>& args) {
    if (should_use_native(APICategory::BATTERY)) {
        BatteryInfo info = NativeAPI::get_battery_info();
        if (info.supported) {
            return Value(info.charging);
        }
    }
    ctx.throw_error("NotSupportedError: Battery API is not available on this platform");
    return Value();
}

Value APIRouter::get_battery_level(Context& ctx, const std::vector<Value>& args) {
    if (should_use_native(APICategory::BATTERY)) {
        BatteryInfo info = NativeAPI::get_battery_info();
        if (info.supported) {
            return Value(info.level);
        }
    }
    ctx.throw_error("NotSupportedError: Battery API is not available on this platform");
    return Value();
}

Value APIRouter::vibrate_device(Context& ctx, const std::vector<Value>& args) {
    if (should_use_native(APICategory::VIBRATION)) {
        if (!args.empty()) {
            std::vector<long> pattern;
            
            if (args[0].is_number()) {
                pattern.push_back(static_cast<long>(args[0].to_number()));
            }
            
            if (!pattern.empty()) {
                bool success = NativeAPI::vibrate(pattern);
                if (success) {
                    return Value(true);
                }
            }
        }
    }
    ctx.throw_error("NotSupportedError: Vibration API is not available on this platform");
    return Value();
}

Value APIRouter::get_battery_charging_time(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Battery charging time API is not available on this platform");
    return Value();
}

Value APIRouter::get_battery_discharging_time(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Battery discharging time API is not available on this platform");
    return Value();
}

Value APIRouter::show_notification(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Notifications API is not available on this platform");
    return Value();
}

Value APIRouter::request_notification_permission(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Notification permission API is not available on this platform");
    return Value();
}

Value APIRouter::get_current_position(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Geolocation API is not available on this platform");
    return Value();
}

Value APIRouter::watch_position(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Geolocation watch position API is not available on this platform");
    return Value();
}

Value APIRouter::clear_watch(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Geolocation clear watch API is not available on this platform");
    return Value();
}

Value APIRouter::get_screen_width(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Screen API should use direct properties, not functions");
    return Value();
}

Value APIRouter::get_screen_height(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Screen API should use direct properties, not functions");
    return Value();
}

Value APIRouter::get_screen_orientation(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Screen orientation API is not available on this platform");
    return Value();
}

Value APIRouter::get_device_pixel_ratio(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_error("NotSupportedError: Device pixel ratio API is not available on this platform");
    return Value();
}

Value APIRouter::read_clipboard_text(Context& ctx, const std::vector<Value>& args) {
    return WebAPI::navigator_clipboard_readText(ctx, args);
}

Value APIRouter::write_clipboard_text(Context& ctx, const std::vector<Value>& args) {
    return WebAPI::navigator_clipboard_writeText(ctx, args);
}

Value APIRouter::speak_text(Context& ctx, const std::vector<Value>& args) {
    return WebAPI::speechSynthesis_speak(ctx, args);
}

Value APIRouter::get_voices(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value APIRouter::get_gamepads(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value APIRouter::get_connection_info(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value APIRouter::is_online(Context& ctx, const std::vector<Value>& args) {
    return Value(true);
}

Value APIRouter::enumerate_devices(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value APIRouter::get_user_media(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value APIRouter::get_navigator_platform(Context& ctx, const std::vector<Value>& args) {
    return Value("Windows");
}

Value APIRouter::get_user_agent(Context& ctx, const std::vector<Value>& args) {
    return Value("Quanta/1.0");
}

Value APIRouter::get_hardware_concurrency(Context& ctx, const std::vector<Value>& args) {
    return Value(4.0);
}

Value APIRouter::performance_now(Context& ctx, const std::vector<Value>& args) {
    return WebAPI::performance_now(ctx, args);
}

Value APIRouter::detect_capabilities(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

}
