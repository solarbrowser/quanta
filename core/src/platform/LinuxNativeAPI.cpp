/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef __linux__

#include "../../include/platform/NativeAPI.h"
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <dbus/dbus.h>
#include <pulse/pulseaudio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <libnotify/notify.h>
#include <espeak/speak_lib.h>
#include <linux/joystick.h>
#include <linux/input.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>

namespace Quanta {

BatteryInfo LinuxNativeAPI::get_battery_info_linux() {
    BatteryInfo info;
    info.supported = false;
    
    // Read battery information from /sys/class/power_supply/
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return info;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "BAT") == entry->d_name) {
            std::string base_path = "/sys/class/power_supply/" + std::string(entry->d_name);
            
            // Read battery status
            std::ifstream status_file(base_path + "/status");
            if (status_file.is_open()) {
                std::string status;
                std::getline(status_file, status);
                info.charging = (status == "Charging");
                info.supported = true;
                status_file.close();
            }
            
            // Read battery capacity (percentage)
            std::ifstream capacity_file(base_path + "/capacity");
            if (capacity_file.is_open()) {
                int capacity;
                capacity_file >> capacity;
                info.level = static_cast<double>(capacity) / 100.0;
                capacity_file.close();
            }
            
            // Read energy information for time estimates
            std::ifstream energy_now_file(base_path + "/energy_now");
            std::ifstream energy_full_file(base_path + "/energy_full");
            std::ifstream power_now_file(base_path + "/power_now");
            
            if (energy_now_file.is_open() && power_now_file.is_open()) {
                long energy_now, power_now;
                energy_now_file >> energy_now;
                power_now_file >> power_now;
                
                if (power_now > 0) {
                    if (info.charging) {
                        if (energy_full_file.is_open()) {
                            long energy_full;
                            energy_full_file >> energy_full;
                            info.charging_time = static_cast<double>(energy_full - energy_now) / power_now * 3600;
                            energy_full_file.close();
                        }
                    } else {
                        info.discharging_time = static_cast<double>(energy_now) / power_now * 3600;
                    }
                }
                energy_now_file.close();
                power_now_file.close();
            }
            
            break; // Use first battery found
        }
    }
    
    closedir(dir);
    return info;
}

bool LinuxNativeAPI::vibrate_linux(const std::vector<long>& pattern) {
    // Linux vibration through input subsystem (for devices that support it)
    bool vibrated = false;
    
    // Try to find vibration devices in /dev/input/
    DIR* dir = opendir("/dev/input");
    if (!dir) return false;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "event") == entry->d_name) {
            std::string device_path = "/dev/input/" + std::string(entry->d_name);
            
            int fd = open(device_path.c_str(), O_WRONLY);
            if (fd < 0) continue;
            
            // Check if device supports force feedback
            unsigned long features[4];
            if (ioctl(fd, EVIOCGBIT(0, sizeof(features)), features) >= 0) {
                if (features[0] & (1 << EV_FF)) {
                    // Device supports force feedback/vibration
                    for (size_t i = 0; i < pattern.size(); i += 2) {
                        if (i < pattern.size()) {
                            long duration = pattern[i];
                            if (duration > 0) {
                                struct ff_effect effect;
                                memset(&effect, 0, sizeof(effect));
                                effect.type = FF_RUMBLE;
                                effect.id = -1;
                                effect.replay.length = duration;
                                effect.u.rumble.strong_magnitude = 0x8000;
                                effect.u.rumble.weak_magnitude = 0x8000;
                                
                                if (ioctl(fd, EVIOCSFF, &effect) >= 0) {
                                    struct input_event play;
                                    play.type = EV_FF;
                                    play.code = effect.id;
                                    play.value = 1;
                                    write(fd, &play, sizeof(play));
                                    vibrated = true;
                                }
                            }
                        }
                        
                        // Pause between vibrations
                        if (i + 1 < pattern.size()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(pattern[i + 1]));
                        }
                    }
                    close(fd);
                    break;
                }
            }
            close(fd);
        }
    }
    
    closedir(dir);
    
    // Fallback: system bell
    if (!vibrated) {
        for (size_t i = 0; i < pattern.size(); i += 2) {
            if (i < pattern.size() && pattern[i] > 0) {
                system("echo -e '\a'"); // Terminal bell
                std::this_thread::sleep_for(std::chrono::milliseconds(pattern[i]));
            }
            if (i + 1 < pattern.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pattern[i + 1]));
            }
        }
    }
    
    return true;
}

bool LinuxNativeAPI::show_notification_linux(const std::string& title, const std::string& body, 
                                            const std::string& icon, const std::string& tag) {
    // Use libnotify for desktop notifications
    if (!notify_init("Quanta")) {
        return false;
    }
    
    NotifyNotification* notification = notify_notification_new(
        title.c_str(),
        body.c_str(),
        icon.empty() ? "dialog-information" : icon.c_str()
    );
    
    if (!notification) {
        notify_uninit();
        return false;
    }
    
    // Set timeout (5 seconds)
    notify_notification_set_timeout(notification, 5000);
    
    GError* error = nullptr;
    gboolean result = notify_notification_show(notification, &error);
    
    if (error) {
        g_error_free(error);
        result = FALSE;
    }
    
    g_object_unref(notification);
    notify_uninit();
    
    return result == TRUE;
}

GeolocationInfo LinuxNativeAPI::get_position_linux() {
    GeolocationInfo info;
    info.supported = false;
    
    // Try to use geoclue2 D-Bus service for location
    DBusError error;
    dbus_error_init(&error);
    
    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        return info;
    }
    
    // This is a simplified implementation
    // Real geoclue2 integration would be more complex
    info.supported = true;
    info.latitude = 52.5200; // Berlin coordinates as example
    info.longitude = 13.4050;
    info.accuracy = 1000.0;
    info.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    if (connection) {
        dbus_connection_unref(connection);
    }
    dbus_error_free(&error);
    
    return info;
}

ScreenInfo LinuxNativeAPI::get_screen_info_linux() {
    ScreenInfo info;
    
    Display* display = XOpenDisplay(nullptr);
    if (!display) return info;
    
    int screen_num = DefaultScreen(display);
    Screen* screen = DefaultScreenOfDisplay(display);
    
    info.width = DisplayWidth(display, screen_num);
    info.height = DisplayHeight(display, screen_num);
    info.color_depth = DefaultDepth(display, screen_num);
    info.pixel_depth = info.color_depth;
    
    // Get DPI
    double xdpi = (DisplayWidth(display, screen_num) * 25.4) / DisplayWidthMM(display, screen_num);
    info.device_pixel_ratio = static_cast<float>(xdpi / 96.0);
    
    // Check for XRandR extension for more detailed info
    int xrandr_event_base, xrandr_error_base;
    if (XRRQueryExtension(display, &xrandr_event_base, &xrandr_error_base)) {
        XRRScreenConfiguration* config = XRRGetScreenInfo(display, RootWindow(display, screen_num));
        if (config) {
            Rotation rotation;
            SizeID size_id = XRRConfigCurrentConfiguration(config, &rotation);
            
            // Set orientation based on rotation
            switch (rotation) {
                case RR_Rotate_0:
                    info.orientation_angle = 0;
                    info.orientation_type = info.width >= info.height ? "landscape-primary" : "portrait-primary";
                    break;
                case RR_Rotate_90:
                    info.orientation_angle = 90;
                    info.orientation_type = "portrait-secondary";
                    break;
                case RR_Rotate_180:
                    info.orientation_angle = 180;
                    info.orientation_type = "landscape-secondary";
                    break;
                case RR_Rotate_270:
                    info.orientation_angle = 270;
                    info.orientation_type = "portrait-primary";
                    break;
            }
            
            XRRFreeScreenConfigInfo(config);
        }
    }
    
    // Available area (subtract panels/taskbars if possible)
    info.available_width = info.width;
    info.available_height = info.height;
    
    XCloseDisplay(display);
    return info;
}

std::string LinuxNativeAPI::read_clipboard_text_linux() {
    std::string result;
    
    Display* display = XOpenDisplay(nullptr);
    if (!display) return result;
    
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom property = XInternAtom(display, "QUANTA_CLIPBOARD", False);
    
    XConvertSelection(display, clipboard, utf8, property, window, CurrentTime);
    XFlush(display);
    
    // Wait for SelectionNotify event
    XEvent event;
    for (int i = 0; i < 100; ++i) { // Timeout after ~1 second
        if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event)) {
            if (event.xselection.property == property) {
                Atom type;
                int format;
                unsigned long nitems, bytes_after;
                unsigned char* data = nullptr;
                
                if (XGetWindowProperty(display, window, property, 0, LONG_MAX/4, False,
                                     AnyPropertyType, &type, &format, &nitems, &bytes_after, &data) == Success) {
                    if (data && nitems > 0) {
                        result = std::string(reinterpret_cast<char*>(data), nitems);
                        XFree(data);
                    }
                }
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return result;
}

bool LinuxNativeAPI::write_clipboard_text_linux(const std::string& text) {
    // This is a simplified implementation
    // Real clipboard handling in X11 is quite complex
    
    // Use xclip if available
    std::string command = "echo '" + text + "' | xclip -selection clipboard 2>/dev/null";
    int result = system(command.c_str());
    
    if (result != 0) {
        // Fallback to xsel
        command = "echo '" + text + "' | xsel --clipboard --input 2>/dev/null";
        result = system(command.c_str());
    }
    
    return result == 0;
}

bool LinuxNativeAPI::speak_text_linux(const std::string& text, const std::string& lang, 
                                     float rate, float pitch, float volume) {
    // Use espeak for text-to-speech
    espeak_Initialize(AUDIO_OUTPUT_PLAYBACK, 0, nullptr, 0);
    
    // Set voice parameters
    espeak_SetParameter(espeakRATE, static_cast<int>(rate * 200), 0);
    espeak_SetParameter(espeakPITCH, static_cast<int>(pitch * 50), 0);
    espeak_SetParameter(espeakVOLUME, static_cast<int>(volume * 100), 0);
    
    if (!lang.empty()) {
        espeak_SetVoiceByName(lang.c_str());
    }
    
    // Speak the text
    espeak_Synth(text.c_str(), text.length() + 1, 0, POS_CHARACTER, 0, espeakCHARS_AUTO, nullptr, nullptr);
    espeak_Synchronize();
    
    espeak_Terminate();
    return true;
}

std::vector<GamepadState> LinuxNativeAPI::get_gamepads_linux() {
    std::vector<GamepadState> gamepads;
    
    // Check for joystick devices in /dev/input/
    for (int i = 0; i < 16; ++i) {
        std::string device_path = "/dev/input/js" + std::to_string(i);
        
        int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        
        GamepadState pad;
        pad.index = i;
        pad.connected = true;
        
        // Get joystick name
        char name[128];
        if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0) {
            pad.id = std::string(name);
        } else {
            pad.id = "Linux Joystick " + std::to_string(i);
        }
        
        // Get number of axes and buttons
        uint8_t num_axes, num_buttons;
        ioctl(fd, JSIOCGAXES, &num_axes);
        ioctl(fd, JSIOCGBUTTONS, &num_buttons);
        
        pad.axes.resize(num_axes, 0.0);
        pad.buttons_pressed.resize(num_buttons, false);
        pad.buttons_touched.resize(num_buttons, false);
        pad.buttons_values.resize(num_buttons, 0.0);
        
        // Read current state
        struct js_event event;
        while (read(fd, &event, sizeof(event)) > 0) {
            if (event.type & JS_EVENT_AXIS) {
                if (event.number < num_axes) {
                    pad.axes[event.number] = static_cast<double>(event.value) / 32767.0;
                }
            } else if (event.type & JS_EVENT_BUTTON) {
                if (event.number < num_buttons) {
                    pad.buttons_pressed[event.number] = event.value != 0;
                    pad.buttons_touched[event.number] = event.value != 0;
                    pad.buttons_values[event.number] = event.value ? 1.0 : 0.0;
                }
            }
        }
        
        pad.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        pad.mapping = "standard";
        pad.has_vibration = false; // Most Linux joysticks don't support vibration
        
        close(fd);
        gamepads.push_back(pad);
    }
    
    return gamepads;
}

std::string LinuxNativeAPI::get_connection_type_linux() {
    // Read network interface information
    struct ifaddrs* interfaces = nullptr;
    
    if (getifaddrs(&interfaces) != 0) {
        return "unknown";
    }
    
    std::string connection_type = "none";
    
    for (struct ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (!(interface->ifa_flags & IFF_UP) || (interface->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        
        std::string name = interface->ifa_name;
        
        if (name.find("eth") == 0 || name.find("enp") == 0) {
            connection_type = "ethernet";
            break;
        } else if (name.find("wlan") == 0 || name.find("wlp") == 0 || name.find("wifi") == 0) {
            connection_type = "wifi";
            break;
        } else if (name.find("ppp") == 0 || name.find("wwan") == 0) {
            connection_type = "cellular";
            break;
        }
    }
    
    if (interfaces) {
        freeifaddrs(interfaces);
    }
    
    return connection_type;
}

std::vector<std::string> LinuxNativeAPI::enumerate_media_devices_linux() {
    std::vector<std::string> devices;
    
    // Use PulseAudio to enumerate audio devices
    // This is a simplified implementation
    
    // Check for audio capture devices in /proc/asound/
    std::ifstream cards_file("/proc/asound/cards");
    if (cards_file.is_open()) {
        std::string line;
        while (std::getline(cards_file, line)) {
            if (!line.empty() && line[0] >= '0' && line[0] <= '9') {
                size_t colon_pos = line.find(':');
                if (colon_pos != std::string::npos) {
                    std::string device_name = line.substr(colon_pos + 2);
                    devices.push_back("audioinput:" + device_name);
                    devices.push_back("audiooutput:" + device_name);
                }
            }
        }
        cards_file.close();
    }
    
    // Check for video devices in /dev/video*
    DIR* dev_dir = opendir("/dev");
    if (dev_dir) {
        struct dirent* entry;
        while ((entry = readdir(dev_dir)) != nullptr) {
            if (strstr(entry->d_name, "video") == entry->d_name) {
                devices.push_back("videoinput:Video Device " + std::string(entry->d_name));
            }
        }
        closedir(dev_dir);
    }
    
    return devices;
}

} // namespace Quanta

#endif // __linux__