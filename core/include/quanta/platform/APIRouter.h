/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_API_ROUTER_H
#define QUANTA_API_ROUTER_H

#include "../Value.h"
#include "../Context.h"
#include "NativeAPI.h"

namespace Quanta {

/**
 * Smart API Router - Decides when to use native APIs vs existing Web API implementations
 * Provides seamless integration between simulated and native functionality
 */
class APIRouter {
public:
    enum class RoutePreference {
        PREFER_NATIVE,
        PREFER_SIMULATED,
        NATIVE_ONLY,
        SIMULATED_ONLY
    };

    enum class APICategory {
        BATTERY,
        VIBRATION,
        NOTIFICATIONS,
        GEOLOCATION,
        SCREEN,
        CLIPBOARD,
        SPEECH_SYNTHESIS,
        SPEECH_RECOGNITION,
        GAMEPAD,
        NETWORK,
        MEDIA_DEVICES,
        SENSORS,
        FILE_SYSTEM,
        CRYPTO,
        PERFORMANCE
    };

    static void initialize(RoutePreference default_preference = RoutePreference::PREFER_NATIVE);
    
    static void set_preference(APICategory category, RoutePreference preference);
    static RoutePreference get_preference(APICategory category);
    
    static bool is_native_available(APICategory category);
    
    
    static Value get_battery_charging(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_level(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_charging_time(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_discharging_time(Context& ctx, const std::vector<Value>& args);
    
    static Value vibrate_device(Context& ctx, const std::vector<Value>& args);
    
    static Value show_notification(Context& ctx, const std::vector<Value>& args);
    static Value request_notification_permission(Context& ctx, const std::vector<Value>& args);
    
    static Value get_current_position(Context& ctx, const std::vector<Value>& args);
    static Value watch_position(Context& ctx, const std::vector<Value>& args);
    static Value clear_watch(Context& ctx, const std::vector<Value>& args);
    
    static Value get_screen_width(Context& ctx, const std::vector<Value>& args);
    static Value get_screen_height(Context& ctx, const std::vector<Value>& args);
    static Value get_screen_orientation(Context& ctx, const std::vector<Value>& args);
    static Value get_device_pixel_ratio(Context& ctx, const std::vector<Value>& args);
    
    static Value read_clipboard_text(Context& ctx, const std::vector<Value>& args);
    static Value write_clipboard_text(Context& ctx, const std::vector<Value>& args);
    
    static Value speak_text(Context& ctx, const std::vector<Value>& args);
    static Value get_voices(Context& ctx, const std::vector<Value>& args);
    
    static Value get_gamepads(Context& ctx, const std::vector<Value>& args);
    
    static Value get_connection_info(Context& ctx, const std::vector<Value>& args);
    static Value is_online(Context& ctx, const std::vector<Value>& args);
    
    static Value enumerate_devices(Context& ctx, const std::vector<Value>& args);
    static Value get_user_media(Context& ctx, const std::vector<Value>& args);
    
    static Value get_navigator_platform(Context& ctx, const std::vector<Value>& args);
    static Value get_user_agent(Context& ctx, const std::vector<Value>& args);
    static Value get_hardware_concurrency(Context& ctx, const std::vector<Value>& args);
    
    static Value performance_now(Context& ctx, const std::vector<Value>& args);
    
    static Value detect_capabilities(Context& ctx, const std::vector<Value>& args);
    
    static bool should_use_native(APICategory category);
    
private:
    static RoutePreference default_preference_;
    static std::map<APICategory, RoutePreference> category_preferences_;
    static bool initialized_;
    
    static Value call_existing_battery_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_vibration_api(Context& ctx, const std::vector<Value>& args);
    static Value call_existing_notification_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_geolocation_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_screen_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_clipboard_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_speech_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_gamepad_api(Context& ctx, const std::vector<Value>& args);
    static Value call_existing_network_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_media_api(Context& ctx, const std::vector<Value>& args, const std::string& method);
    static Value call_existing_performance_api(Context& ctx, const std::vector<Value>& args);
};

}

#endif
