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
    // Configuration flags for API routing
    enum class RoutePreference {
        PREFER_NATIVE,    // Use native APIs when available, fallback to simulation
        PREFER_SIMULATED, // Use existing simulated APIs, only use native when required
        NATIVE_ONLY,      // Only use native APIs, fail if not available
        SIMULATED_ONLY    // Only use simulated APIs, never use native
    };

    // API categories for selective routing
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

    // Initialize the router with preferences
    static void initialize(RoutePreference default_preference = RoutePreference::PREFER_NATIVE);
    
    // Set routing preference for specific API categories
    static void set_preference(APICategory category, RoutePreference preference);
    static RoutePreference get_preference(APICategory category);
    
    // Check if native API is available for category
    static bool is_native_available(APICategory category);
    
    // Smart routing functions - automatically choose best implementation
    
    // Battery API routing
    static Value get_battery_charging(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_level(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_charging_time(Context& ctx, const std::vector<Value>& args);
    static Value get_battery_discharging_time(Context& ctx, const std::vector<Value>& args);
    
    // Vibration API routing
    static Value vibrate_device(Context& ctx, const std::vector<Value>& args);
    
    // Notification API routing  
    static Value show_notification(Context& ctx, const std::vector<Value>& args);
    static Value request_notification_permission(Context& ctx, const std::vector<Value>& args);
    
    // Geolocation API routing
    static Value get_current_position(Context& ctx, const std::vector<Value>& args);
    static Value watch_position(Context& ctx, const std::vector<Value>& args);
    static Value clear_watch(Context& ctx, const std::vector<Value>& args);
    
    // Screen API routing
    static Value get_screen_width(Context& ctx, const std::vector<Value>& args);
    static Value get_screen_height(Context& ctx, const std::vector<Value>& args);
    static Value get_screen_orientation(Context& ctx, const std::vector<Value>& args);
    static Value get_device_pixel_ratio(Context& ctx, const std::vector<Value>& args);
    
    // Clipboard API routing
    static Value read_clipboard_text(Context& ctx, const std::vector<Value>& args);
    static Value write_clipboard_text(Context& ctx, const std::vector<Value>& args);
    
    // Speech API routing
    static Value speak_text(Context& ctx, const std::vector<Value>& args);
    static Value get_voices(Context& ctx, const std::vector<Value>& args);
    
    // Gamepad API routing
    static Value get_gamepads(Context& ctx, const std::vector<Value>& args);
    
    // Network API routing
    static Value get_connection_info(Context& ctx, const std::vector<Value>& args);
    static Value is_online(Context& ctx, const std::vector<Value>& args);
    
    // Media Devices API routing
    static Value enumerate_devices(Context& ctx, const std::vector<Value>& args);
    static Value get_user_media(Context& ctx, const std::vector<Value>& args);
    
    // Platform detection
    static Value get_navigator_platform(Context& ctx, const std::vector<Value>& args);
    static Value get_user_agent(Context& ctx, const std::vector<Value>& args);
    static Value get_hardware_concurrency(Context& ctx, const std::vector<Value>& args);
    
    // Performance routing
    static Value performance_now(Context& ctx, const std::vector<Value>& args);
    
    // Capability detection
    static Value detect_capabilities(Context& ctx, const std::vector<Value>& args);
    
    // Routing decision helper
    static bool should_use_native(APICategory category);
    
private:
    static RoutePreference default_preference_;
    static std::map<APICategory, RoutePreference> category_preferences_;
    static bool initialized_;
    
    // Helper functions to call existing WebAPI implementations
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

} // namespace Quanta

#endif // QUANTA_API_ROUTER_H