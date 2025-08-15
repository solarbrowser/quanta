/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_NATIVE_API_H
#define QUANTA_NATIVE_API_H

#include "../Value.h"
#include "../Context.h"
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace Quanta {

// Platform detection
enum class Platform {
    WINDOWS,
    LINUX,
    MACOS,
    ANDROID,
    IOS,
    UNKNOWN
};

// Device capability flags
enum class DeviceCapability {
    NOTIFICATION_SYSTEM = 1 << 0,
    VIBRATION = 1 << 1,
    BATTERY_INFO = 1 << 2,
    GEOLOCATION = 1 << 3,
    CAMERA = 1 << 4,
    MICROPHONE = 1 << 5,
    CLIPBOARD = 1 << 6,
    FILE_SYSTEM = 1 << 7,
    GAMEPAD = 1 << 8,
    SCREEN_INFO = 1 << 9,
    SPEECH_SYNTHESIS = 1 << 10,
    SPEECH_RECOGNITION = 1 << 11,
    SENSORS = 1 << 12,
    NETWORK_INFO = 1 << 13,
    DEVICE_ORIENTATION = 1 << 14,
    TOUCH_SUPPORT = 1 << 15
};

// Battery information structure
struct BatteryInfo {
    bool charging = false;
    double level = 1.0; // 0.0 to 1.0
    double charging_time = 0; // seconds, Infinity if unknown
    double discharging_time = 0; // seconds, Infinity if unknown
    bool supported = false;
};

// Geolocation information
struct GeolocationInfo {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double accuracy = 0.0;
    double altitude_accuracy = 0.0;
    double heading = 0.0;
    double speed = 0.0;
    long long timestamp = 0;
    bool supported = false;
};

// Screen information
struct ScreenInfo {
    int width = 1920;
    int height = 1080;
    int available_width = 1920;
    int available_height = 1080;
    int color_depth = 24;
    int pixel_depth = 24;
    int orientation_angle = 0;
    std::string orientation_type = "landscape-primary";
    float device_pixel_ratio = 1.0f;
};

// Network information
struct NetworkInfo {
    std::string connection_type = "unknown"; // "wifi", "ethernet", "cellular", "bluetooth", "other", "unknown"
    std::string effective_type = "4g"; // "slow-2g", "2g", "3g", "4g", "5g"
    double downlink = 10.0; // Mbps
    double uplink = 10.0; // Mbps  
    double rtt = 50.0; // Round trip time in ms
    bool metered = false; // Is connection metered/limited
    bool online = true; // Is device online
    std::string ip_address = "";
    std::string mac_address = "";
    std::string ssid = ""; // WiFi network name
    int signal_strength = 100; // 0-100 percentage
    long long bytes_received = 0;
    long long bytes_sent = 0;
    bool supported = false;
};

// Device Orientation information
struct DeviceOrientationInfo {
    double alpha = 0.0;    // Z-axis rotation (compass heading) 0-360 degrees
    double beta = 0.0;     // X-axis rotation (front-to-back tilt) -180 to 180 degrees  
    double gamma = 0.0;    // Y-axis rotation (left-to-right tilt) -90 to 90 degrees
    bool absolute = false; // True if providing absolute values, false if relative
    long long timestamp = 0; // Timestamp in milliseconds
    bool supported = false;
};

// Device Motion information (accelerometer + gyroscope data)
struct DeviceMotionInfo {
    // Acceleration (m/s²)
    double acceleration_x = 0.0;
    double acceleration_y = 0.0; 
    double acceleration_z = 0.0;
    
    // Acceleration including gravity (m/s²)
    double acceleration_including_gravity_x = 0.0;
    double acceleration_including_gravity_y = 0.0;
    double acceleration_including_gravity_z = 0.0;
    
    // Rotation rate (degrees/second)
    double rotation_rate_alpha = 0.0;
    double rotation_rate_beta = 0.0;
    double rotation_rate_gamma = 0.0;
    
    double interval = 16.0; // Update interval in milliseconds
    long long timestamp = 0;
    bool supported = false;
};

// Device information
struct DeviceInfo {
    std::string platform_name;
    std::string user_agent;
    std::string language;
    std::vector<std::string> languages;
    bool online = true;
    int hardware_concurrency = 4;
    long long max_touch_points = 0;
    uint32_t supported_capabilities = 0; // BitMask of DeviceCapability
};

// Gamepad state
struct GamepadState {
    std::string id;
    int index = -1;
    bool connected = false;
    long long timestamp = 0;
    std::string mapping = "standard";
    std::vector<double> axes;
    std::vector<bool> buttons_pressed;
    std::vector<bool> buttons_touched;
    std::vector<double> buttons_values;
    bool has_vibration = false;
};

/**
 * Cross-platform Native API abstraction layer
 * Provides native device functionality for JavaScript Web APIs
 */
class NativeAPI {
public:
    // Platform detection and initialization
    static Platform detect_platform();
    static bool initialize_platform_apis();
    static void shutdown_platform_apis();
    static DeviceInfo get_device_info();
    static uint32_t get_device_capabilities();
    
    // Battery API - Native system integration
    static BatteryInfo get_battery_info();
    static bool register_battery_change_callback(std::function<void(const BatteryInfo&)> callback);
    
    // Vibration API - Native haptic feedback
    static bool vibrate(const std::vector<long>& pattern);
    static bool cancel_vibration();
    
    // Notification API - Native system notifications
    static bool show_notification(const std::string& title, const std::string& body, 
                                 const std::string& icon = "", const std::string& tag = "");
    static bool request_notification_permission();
    static std::string get_notification_permission();
    static bool close_notification(const std::string& tag);
    
    // Geolocation API - Native GPS/location services
    static GeolocationInfo get_current_position(bool high_accuracy = false);
    static int watch_position(std::function<void(const GeolocationInfo&)> success_callback,
                             std::function<void(const std::string&)> error_callback,
                             bool high_accuracy = false);
    static bool clear_watch_position(int watch_id);
    
    // Screen API - Native display information
    static ScreenInfo get_screen_info();
    static bool lock_screen_orientation(const std::string& orientation);
    static bool unlock_screen_orientation();
    
    // Clipboard API - Native clipboard access
    static std::string read_clipboard_text();
    static bool write_clipboard_text(const std::string& text);
    static std::vector<uint8_t> read_clipboard_data(const std::string& mime_type);
    static bool write_clipboard_data(const std::string& mime_type, const std::vector<uint8_t>& data);
    
    // File System API - Native file operations
    static std::vector<uint8_t> read_file(const std::string& path);
    static bool write_file(const std::string& path, const std::vector<uint8_t>& data);
    static bool file_exists(const std::string& path);
    static bool create_directory(const std::string& path);
    static bool delete_file(const std::string& path);
    static std::vector<std::string> list_directory(const std::string& path);
    
    // Speech Synthesis API - Native text-to-speech
    static bool speak_text(const std::string& text, const std::string& lang = "",
                          float rate = 1.0f, float pitch = 1.0f, float volume = 1.0f);
    static bool stop_speaking();
    static bool pause_speaking();
    static bool resume_speaking();
    static std::vector<std::string> get_available_voices();
    
    // Speech Recognition API - Native voice recognition
    static bool start_speech_recognition(const std::string& lang = "");
    static bool stop_speech_recognition();
    static bool abort_speech_recognition();
    static bool set_speech_recognition_callback(std::function<void(const std::string&, bool)> callback);
    
    // Gamepad API - Native controller support
    static std::vector<GamepadState> get_gamepads();
    static bool gamepad_vibrate(int gamepad_index, double strong_magnitude, double weak_magnitude, long duration);
    static bool register_gamepad_callback(std::function<void(const GamepadState&, bool)> callback);
    
    // Network Information API - Native network status
    static NetworkInfo get_network_info(); // Comprehensive network information
    static std::string get_connection_type(); // "wifi", "cellular", "ethernet", "none", etc.
    static bool is_online();
    static double get_download_speed(); // Mbps
    static double get_upload_speed(); // Mbps
    static bool is_metered_connection();
    static bool register_network_change_callback(std::function<void(const NetworkInfo&)> callback);
    
    // Device Orientation API - Native sensor data
    static DeviceOrientationInfo get_device_orientation(); // Get current orientation data
    static DeviceMotionInfo get_device_motion(); // Get current motion data
    static bool start_device_orientation(std::function<void(const DeviceOrientationInfo&)> callback);
    static bool stop_device_orientation();
    static bool start_device_motion(std::function<void(const DeviceMotionInfo&)> callback);
    static bool stop_device_motion();
    static bool has_orientation_sensor();
    static bool has_motion_sensor();
    
    // Camera/Media API - Native camera access
    static std::vector<std::string> enumerate_media_devices();
    static bool request_camera_permission();
    static bool request_microphone_permission();
    static bool has_camera();
    static bool has_microphone();
    
    // Platform-specific implementations
    #ifdef _WIN32
    static class WindowsNativeAPI* get_windows_api();
    #endif
    
    #ifdef __linux__
    static class LinuxNativeAPI* get_linux_api();
    #endif
    
    #ifdef __APPLE__
    #ifdef TARGET_OS_IOS
    static class iOSNativeAPI* get_ios_api();
    #else
    static class macOSNativeAPI* get_macos_api();
    #endif
    #endif
    
    #ifdef __ANDROID__
    static class AndroidNativeAPI* get_android_api();
    #endif

private:
    static Platform current_platform_;
    static DeviceInfo device_info_;
    static bool initialized_;
    static std::map<int, std::function<void(const GeolocationInfo&)>> geolocation_watchers_;
    static int next_watch_id_;
    
    // Platform-specific helper functions
    static void initialize_windows_apis();
    static void initialize_linux_apis();
    static void initialize_macos_apis();
    static void initialize_android_apis();
    static void initialize_ios_apis();
};

// Platform-specific API classes (forward declarations)
#ifdef _WIN32
class WindowsNativeAPI {
public:
    static BatteryInfo get_battery_info_windows();
    static bool vibrate_windows(const std::vector<long>& pattern);
    static bool show_notification_windows(const std::string& title, const std::string& body, const std::string& icon, const std::string& tag);
    static GeolocationInfo get_position_windows();
    static ScreenInfo get_screen_info_windows();
    static std::string read_clipboard_text_windows();
    static bool write_clipboard_text_windows(const std::string& text);
    static bool speak_text_windows(const std::string& text, const std::string& lang, float rate, float pitch, float volume);
    static std::vector<GamepadState> get_gamepads_windows();
    static std::string get_connection_type_windows();
    static std::vector<std::string> enumerate_media_devices_windows();
};
#endif

#ifdef __linux__
class LinuxNativeAPI {
public:
    static BatteryInfo get_battery_info_linux();
    static bool vibrate_linux(const std::vector<long>& pattern);
    static bool show_notification_linux(const std::string& title, const std::string& body, const std::string& icon, const std::string& tag);
    static GeolocationInfo get_position_linux();
    static ScreenInfo get_screen_info_linux();
    static std::string read_clipboard_text_linux();
    static bool write_clipboard_text_linux(const std::string& text);
    static bool speak_text_linux(const std::string& text, const std::string& lang, float rate, float pitch, float volume);
    static std::vector<GamepadState> get_gamepads_linux();
    static std::string get_connection_type_linux();
    static std::vector<std::string> enumerate_media_devices_linux();
};
#endif

#ifdef __APPLE__
#ifndef TARGET_OS_IOS
class macOSNativeAPI {
public:
    static BatteryInfo get_battery_info_macos();
    static bool vibrate_macos(const std::vector<long>& pattern);
    static bool show_notification_macos(const std::string& title, const std::string& body, const std::string& icon, const std::string& tag);
    static GeolocationInfo get_position_macos();
    static ScreenInfo get_screen_info_macos();
    static std::string read_clipboard_text_macos();
    static bool write_clipboard_text_macos(const std::string& text);
    static bool speak_text_macos(const std::string& text, const std::string& lang, float rate, float pitch, float volume);
    static std::vector<GamepadState> get_gamepads_macos();
    static std::string get_connection_type_macos();
    static std::vector<std::string> enumerate_media_devices_macos();
};
#else
class iOSNativeAPI {
public:
    static BatteryInfo get_battery_info_ios();
    static bool vibrate_ios(const std::vector<long>& pattern);
    static bool show_notification_ios(const std::string& title, const std::string& body, const std::string& icon, const std::string& tag);
    static GeolocationInfo get_position_ios();
    static ScreenInfo get_screen_info_ios();
    static std::string read_clipboard_text_ios();
    static bool write_clipboard_text_ios(const std::string& text);
    static bool speak_text_ios(const std::string& text, const std::string& lang, float rate, float pitch, float volume);
    static std::vector<GamepadState> get_gamepads_ios();
    static std::string get_connection_type_ios();
    static std::vector<std::string> enumerate_media_devices_ios();
};
#endif
#endif

#ifdef __ANDROID__
class AndroidNativeAPI {
public:
    static BatteryInfo get_battery_info_android();
    static bool vibrate_android(const std::vector<long>& pattern);
    static bool show_notification_android(const std::string& title, const std::string& body, const std::string& icon, const std::string& tag);
    static GeolocationInfo get_position_android();
    static ScreenInfo get_screen_info_android();
    static std::string read_clipboard_text_android();
    static bool write_clipboard_text_android(const std::string& text);
    static bool speak_text_android(const std::string& text, const std::string& lang, float rate, float pitch, float volume);
    static std::vector<GamepadState> get_gamepads_android();
    static std::string get_connection_type_android();
    static std::vector<std::string> enumerate_media_devices_android();
};
#endif

} // namespace Quanta

#endif // QUANTA_NATIVE_API_H