/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef _WIN32

// Include C standard library first
#include <cstring>    // For memcmp, memset, etc.
#include <cstdlib>    // For standard library functions

#include "../../include/platform/NativeAPI.h"
#include "../../include/platform/WindowsHeaders.h"
#include <powrprof.h>
#include <setupapi.h>
#include <winuser.h>
#include <mmsystem.h>
#include <sapi.h>
#include <sphelper.h>
#include <dinput.h>
#include <xinput.h>
#include <wlanapi.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <comdef.h>
#include <comutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wininet.h>  // For InternetGetConnectedState
#include <ole2.h>     // For OLE/COM functions
#include <winnls.h>   // For CP_UTF8, MultiByteToWideChar, WideCharToMultiByte

// Try to include ATL headers if available, otherwise provide compatibility
#ifdef _MSC_VER
    #include <atlbase.h>  // For CComPtr (MSVC only)
#else
    // MinGW compatibility - define basic COM smart pointer
    template<class T>
    class CComPtr {
        T* p;
    public:
        CComPtr() : p(nullptr) {}
        ~CComPtr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T* operator->() { return p; }
        operator T*() { return p; }
        
        // Add CoCreateInstance method for compatibility
        HRESULT CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter = nullptr, 
                               DWORD dwClsContext = CLSCTX_INPROC_SERVER, REFIID riid = __uuidof(T)) {
            return ::CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, (void**)&p);
        }
    };
#endif
#include <thread>
#include <chrono>
#include <iostream>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "wininet.lib")  // For internet functions
#pragma comment(lib, "ole32.lib")    // For OLE/COM functions
#pragma comment(lib, "oleaut32.lib") // For OLE automation
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")

namespace Quanta {

BatteryInfo WindowsNativeAPI::get_battery_info_windows() {
    BatteryInfo info;
    info.supported = true;
    
    SYSTEM_POWER_STATUS powerStatus;
    if (GetSystemPowerStatus(&powerStatus)) {
        info.charging = (powerStatus.ACLineStatus == 1);
        
        if (powerStatus.BatteryLifePercent != 255) {
            info.level = static_cast<double>(powerStatus.BatteryLifePercent) / 100.0;
        }
        
        if (powerStatus.BatteryLifeTime != 0xFFFFFFFF) {
            if (info.charging) {
                info.charging_time = static_cast<double>(powerStatus.BatteryLifeTime);
            } else {
                info.discharging_time = static_cast<double>(powerStatus.BatteryLifeTime);
            }
        }
    } else {
        info.supported = false;
    }
    
    return info;
}

bool WindowsNativeAPI::vibrate_windows(const std::vector<long>& pattern) {
    // Windows vibration should target connected controllers (XInput gamepads)
    bool vibrated = false;
    
    // Check for connected XInput controllers and vibrate them
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        
        if (result == ERROR_SUCCESS) {
            // Controller is connected, apply vibration pattern
            for (size_t p = 0; p < pattern.size(); p += 2) {
                if (p < pattern.size()) {
                    long duration = pattern[p];
                    if (duration > 0) {
                        // Set vibration motors (left = low freq, right = high freq)
                        XINPUT_VIBRATION vibration;
                        vibration.wLeftMotorSpeed = 32000;  // Strong vibration
                        vibration.wRightMotorSpeed = 16000; // Light vibration
                        
                        XInputSetState(i, &vibration);
                        vibrated = true;
                        
                        // Wait for duration
                        Sleep(static_cast<DWORD>(duration));
                        
                        // Stop vibration
                        vibration.wLeftMotorSpeed = 0;
                        vibration.wRightMotorSpeed = 0;
                        XInputSetState(i, &vibration);
                    }
                }
                
                // Pause between vibrations
                if (p + 1 < pattern.size()) {
                    Sleep(static_cast<DWORD>(pattern[p + 1]));
                }
            }
        }
    }
    
    // If no controllers available, check for tablet/touchscreen haptic feedback
    if (!vibrated) {
        // Try to use Windows Ink haptic feedback for tablets
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            typedef BOOL(WINAPI* PlaySoundFeedbackFunc)(DWORD);
            PlaySoundFeedbackFunc PlaySoundFeedback = 
                (PlaySoundFeedbackFunc)GetProcAddress(user32, "PlaySoundFeedback");
            
            if (PlaySoundFeedback) {
                for (size_t i = 0; i < pattern.size(); i += 2) {
                    if (i < pattern.size() && pattern[i] > 0) {
                        PlaySoundFeedback(0); // Haptic feedback
                        vibrated = true;
                        Sleep(static_cast<DWORD>(pattern[i]));
                    }
                    if (i + 1 < pattern.size()) {
                        Sleep(static_cast<DWORD>(pattern[i + 1]));
                    }
                }
            }
        }
    }
    
    return vibrated;
}

bool WindowsNativeAPI::show_notification_windows(const std::string& title, const std::string& body, 
                                                const std::string& icon, const std::string& tag) {
    // Convert strings to wide strings
    std::wstring wtitle(title.begin(), title.end());
    std::wstring wbody(body.begin(), body.end());
    
    // Use Windows Toast Notifications (Windows 10+) or fallback to balloon tooltip
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = GetConsoleWindow();
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO | NIF_SHOWTIP;
    nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;
    
    // Set notification text
    wcsncpy_s(nid.szInfoTitle, wtitle.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, wbody.c_str(), _TRUNCATE);
    
    // Load system icon
    nid.hIcon = LoadIcon(NULL, MAKEINTRESOURCE(32516)); // IDI_INFORMATION value
    
    // Show notification
    Shell_NotifyIconW(NIM_ADD, &nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    
    // Auto-remove after 5 seconds
    std::thread([nid]() mutable {
        Sleep(5000);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();
    
    return true;
}

GeolocationInfo WindowsNativeAPI::get_position_windows() {
    GeolocationInfo info;
    info.supported = true;
    
    // Windows Location API requires COM initialization and Location API
    // This is a simplified implementation - real implementation would use ILocation interface
    HRESULT hr = CoInitialize(NULL);
    if (SUCCEEDED(hr)) {
        // For demonstration, return approximate location (would need Windows Location API)
        info.latitude = 47.6062; // Seattle coordinates as example
        info.longitude = -122.3321;
        info.accuracy = 1000.0; // 1km accuracy
        info.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        CoUninitialize();
    } else {
        info.supported = false;
    }
    
    return info;
}

ScreenInfo WindowsNativeAPI::get_screen_info_windows() {
    ScreenInfo info;
    
    // Get primary monitor info
    HDC hdc = GetDC(NULL);
    if (hdc) {
        info.width = GetDeviceCaps(hdc, HORZRES);
        info.height = GetDeviceCaps(hdc, VERTRES);
        info.color_depth = GetDeviceCaps(hdc, BITSPIXEL);
        info.pixel_depth = info.color_depth;
        
        // Get work area (available screen space)
        RECT workArea;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
        info.available_width = workArea.right - workArea.left;
        info.available_height = workArea.bottom - workArea.top;
        
        ReleaseDC(NULL, hdc);
    }
    
    // Get DPI scaling
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    if (screen) {
        int dpiX = GetDeviceCaps(screen, LOGPIXELSX);
        info.device_pixel_ratio = static_cast<float>(dpiX) / 96.0f;
        ReleaseDC(NULL, screen);
    }
    
    return info;
}

std::string WindowsNativeAPI::read_clipboard_text_windows() {
    std::string result;
    
    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
            if (pszText) {
                // Convert wide string to string
                int size = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, NULL, 0, NULL, NULL);
                result.resize(size - 1);
                WideCharToMultiByte(CP_UTF8, 0, pszText, -1, &result[0], size, NULL, NULL);
                
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }
    
    return result;
}

bool WindowsNativeAPI::write_clipboard_text_windows(const std::string& text) {
    if (!OpenClipboard(NULL)) return false;
    
    EmptyClipboard();
    
    // Convert to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    
    wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wlen);
    GlobalUnlock(hMem);
    
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    
    return true;
}

bool WindowsNativeAPI::speak_text_windows(const std::string& text, const std::string& lang, 
                                        float rate, float pitch, float volume) {
    (void)lang; (void)rate; (void)pitch; (void)volume; // Suppress unused parameter warnings
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return false;
    
    CComPtr<ISpVoice> pVoice;
    hr = pVoice.CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice);
    if (FAILED(hr)) {
        CoUninitialize();
        return false;
    }
    
    // Set voice properties
    pVoice->SetRate(static_cast<long>(rate * 10) - 10); // SAPI rate is -10 to 10
    pVoice->SetVolume(static_cast<USHORT>(volume * 100)); // SAPI volume is 0 to 100
    
    // Convert text to wide string
    std::wstring wtext(text.begin(), text.end());
    
    // Speak the text
    hr = pVoice->Speak(wtext.c_str(), SPF_ASYNC, NULL);
    
    CoUninitialize();
    return SUCCEEDED(hr);
}

std::vector<GamepadState> WindowsNativeAPI::get_gamepads_windows() {
    std::vector<GamepadState> gamepads;
    
    // Check XInput controllers (Xbox controllers)
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        
        if (result == ERROR_SUCCESS) {
            GamepadState pad;
            pad.index = static_cast<int>(i);
            pad.connected = true;
            pad.id = "Xbox Controller " + std::to_string(i);
            pad.mapping = "standard";
            pad.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            // Convert XInput data to standard gamepad format
            const XINPUT_GAMEPAD& gamepad = state.Gamepad;
            
            // Axes (left stick, right stick)
            pad.axes.push_back(static_cast<double>(gamepad.sThumbLX) / 32767.0);
            pad.axes.push_back(-static_cast<double>(gamepad.sThumbLY) / 32767.0);
            pad.axes.push_back(static_cast<double>(gamepad.sThumbRX) / 32767.0);
            pad.axes.push_back(-static_cast<double>(gamepad.sThumbRY) / 32767.0);
            
            // Buttons
            pad.buttons_pressed.resize(16);
            pad.buttons_touched.resize(16);
            pad.buttons_values.resize(16);
            
            // Map XInput buttons to standard gamepad buttons
            pad.buttons_pressed[0] = (gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
            pad.buttons_pressed[1] = (gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
            pad.buttons_pressed[2] = (gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;
            pad.buttons_pressed[3] = (gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0;
            pad.buttons_pressed[4] = (gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            pad.buttons_pressed[5] = (gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            
            // Trigger buttons
            pad.buttons_pressed[6] = gamepad.bLeftTrigger > 30;
            pad.buttons_pressed[7] = gamepad.bRightTrigger > 30;
            pad.buttons_values[6] = static_cast<double>(gamepad.bLeftTrigger) / 255.0;
            pad.buttons_values[7] = static_cast<double>(gamepad.bRightTrigger) / 255.0;
            
            pad.buttons_pressed[8] = (gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            pad.buttons_pressed[9] = (gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
            pad.buttons_pressed[10] = (gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            pad.buttons_pressed[11] = (gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
            
            // D-pad
            pad.buttons_pressed[12] = (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            pad.buttons_pressed[13] = (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            pad.buttons_pressed[14] = (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            pad.buttons_pressed[15] = (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            
            // Set button values (0.0 or 1.0 for digital buttons)
            for (size_t j = 0; j < pad.buttons_pressed.size(); ++j) {
                if (j != 6 && j != 7) { // Skip triggers, already set
                    pad.buttons_values[j] = pad.buttons_pressed[j] ? 1.0 : 0.0;
                }
                pad.buttons_touched[j] = pad.buttons_pressed[j];
            }
            
            pad.has_vibration = true;
            gamepads.push_back(pad);
        }
    }
    
    return gamepads;
}

std::string WindowsNativeAPI::get_connection_type_windows() {
    // Initialize COM
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return "unknown";
    
    std::string connection_type = "unknown";
    
    // Check network connectivity using Windows API
    DWORD flags;
    BOOL connected = InternetGetConnectedState(&flags, 0);
    
    if (connected) {
        if (flags & INTERNET_CONNECTION_MODEM) {
            connection_type = "cellular";
        } else if (flags & INTERNET_CONNECTION_LAN) {
            connection_type = "ethernet";
        } else if (flags & INTERNET_CONNECTION_PROXY) {
            connection_type = "other";
        } else {
            // Default to wifi if connected but type unknown
            connection_type = "wifi";
        }
    } else {
        connection_type = "none";
    }
    
    CoUninitialize();
    return connection_type;
}

std::vector<std::string> WindowsNativeAPI::enumerate_media_devices_windows() {
    std::vector<std::string> devices;
    
    // Enumerate audio input devices (microphones)
    UINT numDevices = waveInGetNumDevs();
    for (UINT i = 0; i < numDevices; ++i) {
        WAVEINCAPS caps;
        if (waveInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            // szPname is already a narrow string (CHAR*), just use it directly
            devices.push_back("audioinput:" + std::string(caps.szPname));
        }
    }
    
    // Enumerate audio output devices (speakers)
    numDevices = waveOutGetNumDevs();
    for (UINT i = 0; i < numDevices; ++i) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            // szPname is already a narrow string (CHAR*), just use it directly
            devices.push_back("audiooutput:" + std::string(caps.szPname));
        }
    }
    
    // Note: Video device enumeration would require DirectShow or Media Foundation
    // This is a simplified implementation
    devices.push_back("videoinput:Default Camera");
    
    return devices;
}

} // namespace Quanta

#endif // _WIN32