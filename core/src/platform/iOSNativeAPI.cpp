/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#if defined(__APPLE__) && defined(TARGET_OS_IOS)

#include "quanta/platform/NativeAPI.h"
#include <UIKit/UIKit.h>
#include <CoreLocation/CoreLocation.h>
#include <UserNotifications/UserNotifications.h>
#include <AVFoundation/AVFoundation.h>
#include <Speech/Speech.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreMotion/CoreMotion.h>
#include <CoreTelephony/CTTelephonyNetworkInfo.h>
#include <CoreTelephony/CTCarrier.h>
#include <GameController/GameController.h>
#include <LocalAuthentication/LocalAuthentication.h>
#include <MediaPlayer/MediaPlayer.h>
#include <thread>
#include <chrono>
#include <iostream>

namespace Quanta {

BatteryInfo iOSNativeAPI::get_battery_info_ios() {
    BatteryInfo info;
    info.supported = true;
    
    @autoreleasepool {
        UIDevice* device = [UIDevice currentDevice];
        device.batteryMonitoringEnabled = YES;
        
        info.level = static_cast<double>(device.batteryLevel);
        
        switch (device.batteryState) {
            case UIDeviceBatteryStateCharging:
                info.charging = true;
                break;
            case UIDeviceBatteryStateFull:
                info.charging = false;
                info.level = 1.0;
                break;
            case UIDeviceBatteryStateUnplugged:
                info.charging = false;
                break;
            case UIDeviceBatteryStateUnknown:
            default:
                info.supported = false;
                break;
        }
        
        info.charging_time = INFINITY;
        info.discharging_time = INFINITY;
    }
    
    return info;
}

bool iOSNativeAPI::vibrate_ios(const std::vector<long>& pattern) {
    @autoreleasepool {
        for (size_t i = 0; i < pattern.size(); i += 2) {
            if (i < pattern.size()) {
                long duration = pattern[i];
                if (duration > 0) {
                    if (@available(iOS 10.0, *)) {
                        UIImpactFeedbackGenerator* generator = [[UIImpactFeedbackGenerator alloc] 
                                                               initWithStyle:UIImpactFeedbackStyleMedium];
                        [generator impactOccurred];
                    } else {
                        AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                }
            }
            
            if (i + 1 < pattern.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pattern[i + 1]));
            }
        }
    }
    
    return true;
}

bool iOSNativeAPI::show_notification_ios(const std::string& title, const std::string& body, 
                                        const std::string& icon, const std::string& tag) {
    @autoreleasepool {
        if (@available(iOS 10.0, *)) {
            UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
            
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
                                    completionHandler:^(BOOL granted, NSError * _Nullable error) {
                if (granted) {
                    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
                    content.title = [NSString stringWithUTF8String:title.c_str()];
                    content.body = [NSString stringWithUTF8String:body.c_str()];
                    content.sound = [UNNotificationSound defaultSound];
                    
                    NSString* identifier = tag.empty() ? [[NSUUID UUID] UUIDString] : 
                                          [NSString stringWithUTF8String:tag.c_str()];
                    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                            content:content
                                                                                            trigger:nil];
                    
                    [center addNotificationRequest:request withCompletionHandler:^(NSError * _Nullable error) {
                        if (error) {
                            NSLog(@"Notification error: %@", error.localizedDescription);
                        }
                    }];
                }
            }];
        } else {
            UILocalNotification* notification = [[UILocalNotification alloc] init];
            notification.alertTitle = [NSString stringWithUTF8String:title.c_str()];
            notification.alertBody = [NSString stringWithUTF8String:body.c_str()];
            notification.soundName = UILocalNotificationDefaultSoundName;
            
            [[UIApplication sharedApplication] presentLocalNotificationNow:notification];
        }
    }
    
    return true;
}

GeolocationInfo iOSNativeAPI::get_position_ios() {
    GeolocationInfo info;
    info.supported = true;
    
    info.latitude = 37.7749;
    info.longitude = -122.4194;
    info.accuracy = 1000.0;
    info.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return info;
}

ScreenInfo iOSNativeAPI::get_screen_info_ios() {
    ScreenInfo info;
    
    @autoreleasepool {
        UIScreen* mainScreen = [UIScreen mainScreen];
        CGRect bounds = mainScreen.bounds;
        CGFloat scale = mainScreen.scale;
        
        info.width = static_cast<int>(bounds.size.width * scale);
        info.height = static_cast<int>(bounds.size.height * scale);
        info.device_pixel_ratio = static_cast<float>(scale);
        
        UIWindow* keyWindow = nil;
        if (@available(iOS 13.0, *)) {
            for (UIWindowScene* windowScene in [UIApplication sharedApplication].connectedScenes) {
                if (windowScene.activationState == UISceneActivationStateForegroundActive) {
                    keyWindow = windowScene.windows.firstObject;
                    break;
                }
            }
        } else {
            keyWindow = [UIApplication sharedApplication].keyWindow;
        }
        
        if (keyWindow) {
            CGRect safeArea = keyWindow.safeAreaLayoutGuide.layoutFrame;
            info.available_width = static_cast<int>(safeArea.size.width * scale);
            info.available_height = static_cast<int>(safeArea.size.height * scale);
        } else {
            info.available_width = info.width;
            info.available_height = info.height;
        }
        
        UIInterfaceOrientation orientation = [UIApplication sharedApplication].statusBarOrientation;
        switch (orientation) {
            case UIInterfaceOrientationPortrait:
                info.orientation_angle = 0;
                info.orientation_type = "portrait-primary";
                break;
            case UIInterfaceOrientationPortraitUpsideDown:
                info.orientation_angle = 180;
                info.orientation_type = "portrait-secondary";
                break;
            case UIInterfaceOrientationLandscapeLeft:
                info.orientation_angle = 90;
                info.orientation_type = "landscape-secondary";
                break;
            case UIInterfaceOrientationLandscapeRight:
                info.orientation_angle = -90;
                info.orientation_type = "landscape-primary";
                break;
            default:
                info.orientation_angle = 0;
                info.orientation_type = "portrait-primary";
                break;
        }
        
        info.color_depth = 24;
        info.pixel_depth = 24;
    }
    
    return info;
}

std::string iOSNativeAPI::read_clipboard_text_ios() {
    std::string result;
    
    @autoreleasepool {
        UIPasteboard* pasteboard = [UIPasteboard generalPasteboard];
        NSString* string = pasteboard.string;
        
        if (string) {
            result = [string UTF8String];
        }
    }
    
    return result;
}

bool iOSNativeAPI::write_clipboard_text_ios(const std::string& text) {
    @autoreleasepool {
        UIPasteboard* pasteboard = [UIPasteboard generalPasteboard];
        pasteboard.string = [NSString stringWithUTF8String:text.c_str()];
        
        return pasteboard.string != nil;
    }
}

bool iOSNativeAPI::speak_text_ios(const std::string& text, const std::string& lang, 
                                 float rate, float pitch, float volume) {
    @autoreleasepool {
        AVSpeechSynthesizer* synthesizer = [[AVSpeechSynthesizer alloc] init];
        
        AVSpeechUtterance* utterance = [AVSpeechUtterance speechUtteranceWithString:
                                       [NSString stringWithUTF8String:text.c_str()]];
        
        utterance.rate = rate * AVSpeechUtteranceDefaultSpeechRate;
        utterance.pitchMultiplier = pitch;
        utterance.volume = volume;
        
        if (!lang.empty()) {
            AVSpeechSynthesisVoice* voice = [AVSpeechSynthesisVoice voiceWithLanguage:
                                            [NSString stringWithUTF8String:lang.c_str()]];
            if (voice) {
                utterance.voice = voice;
            }
        }
        
        [synthesizer speakUtterance:utterance];
        
        return true;
    }
}

std::vector<GamepadState> iOSNativeAPI::get_gamepads_ios() {
    std::vector<GamepadState> gamepads;
    
    @autoreleasepool {
        if (@available(iOS 7.0, *)) {
            NSArray<GCController*>* controllers = [GCController controllers];
            
            for (NSUInteger i = 0; i < controllers.count; ++i) {
                GCController* controller = controllers[i];
                
                GamepadState pad;
                pad.index = static_cast<int>(i);
                pad.connected = controller.isAttachedToDevice;
                pad.id = std::string([controller.vendorName UTF8String]);
                pad.mapping = "standard";
                pad.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                
                if (controller.extendedGamepad) {
                    GCExtendedGamepad* gamepad = controller.extendedGamepad;
                    
                    pad.axes.push_back(static_cast<double>(gamepad.leftThumbstick.xAxis.value));
                    pad.axes.push_back(-static_cast<double>(gamepad.leftThumbstick.yAxis.value));
                    
                    pad.axes.push_back(static_cast<double>(gamepad.rightThumbstick.xAxis.value));
                    pad.axes.push_back(-static_cast<double>(gamepad.rightThumbstick.yAxis.value));
                    
                    pad.buttons_pressed.resize(16, false);
                    pad.buttons_touched.resize(16, false);
                    pad.buttons_values.resize(16, 0.0);
                    
                    pad.buttons_pressed[0] = gamepad.buttonA.isPressed;
                    pad.buttons_values[0] = static_cast<double>(gamepad.buttonA.value);
                    pad.buttons_touched[0] = gamepad.buttonA.isPressed;
                    
                    pad.buttons_pressed[1] = gamepad.buttonB.isPressed;
                    pad.buttons_values[1] = static_cast<double>(gamepad.buttonB.value);
                    pad.buttons_touched[1] = gamepad.buttonB.isPressed;
                    
                    pad.buttons_pressed[2] = gamepad.buttonX.isPressed;
                    pad.buttons_values[2] = static_cast<double>(gamepad.buttonX.value);
                    pad.buttons_touched[2] = gamepad.buttonX.isPressed;
                    
                    pad.buttons_pressed[3] = gamepad.buttonY.isPressed;
                    pad.buttons_values[3] = static_cast<double>(gamepad.buttonY.value);
                    pad.buttons_touched[3] = gamepad.buttonY.isPressed;
                    
                    pad.buttons_pressed[4] = gamepad.leftShoulder.isPressed;
                    pad.buttons_values[4] = static_cast<double>(gamepad.leftShoulder.value);
                    pad.buttons_touched[4] = gamepad.leftShoulder.isPressed;
                    
                    pad.buttons_pressed[5] = gamepad.rightShoulder.isPressed;
                    pad.buttons_values[5] = static_cast<double>(gamepad.rightShoulder.value);
                    pad.buttons_touched[5] = gamepad.rightShoulder.isPressed;
                    
                    pad.buttons_pressed[6] = gamepad.leftTrigger.value > 0.1f;
                    pad.buttons_values[6] = static_cast<double>(gamepad.leftTrigger.value);
                    pad.buttons_touched[6] = gamepad.leftTrigger.value > 0.0f;
                    
                    pad.buttons_pressed[7] = gamepad.rightTrigger.value > 0.1f;
                    pad.buttons_values[7] = static_cast<double>(gamepad.rightTrigger.value);
                    pad.buttons_touched[7] = gamepad.rightTrigger.value > 0.0f;
                    
                    if (gamepad.dpad) {
                        pad.buttons_pressed[12] = gamepad.dpad.up.isPressed;
                        pad.buttons_values[12] = static_cast<double>(gamepad.dpad.up.value);
                        pad.buttons_touched[12] = gamepad.dpad.up.isPressed;
                        
                        pad.buttons_pressed[13] = gamepad.dpad.down.isPressed;
                        pad.buttons_values[13] = static_cast<double>(gamepad.dpad.down.value);
                        pad.buttons_touched[13] = gamepad.dpad.down.isPressed;
                        
                        pad.buttons_pressed[14] = gamepad.dpad.left.isPressed;
                        pad.buttons_values[14] = static_cast<double>(gamepad.dpad.left.value);
                        pad.buttons_touched[14] = gamepad.dpad.left.isPressed;
                        
                        pad.buttons_pressed[15] = gamepad.dpad.right.isPressed;
                        pad.buttons_values[15] = static_cast<double>(gamepad.dpad.right.value);
                        pad.buttons_touched[15] = gamepad.dpad.right.isPressed;
                    }
                }
                
                pad.has_vibration = (controller.haptics != nil);
                
                gamepads.push_back(pad);
            }
        }
    }
    
    return gamepads;
}

std::string iOSNativeAPI::get_connection_type_ios() {
    std::string connection_type = "unknown";
    
    @autoreleasepool {
        CTTelephonyNetworkInfo* networkInfo = [[CTTelephonyNetworkInfo alloc] init];
        
        if (@available(iOS 12.0, *)) {
            if (networkInfo.serviceCurrentRadioAccessTechnology) {
                NSString* radioTech = networkInfo.serviceCurrentRadioAccessTechnology.allValues.firstObject;
                
                if ([radioTech isEqualToString:CTRadioAccessTechnologyLTE] ||
                    [radioTech isEqualToString:@"CTRadioAccessTechnology5GNR"]) {
                    connection_type = "cellular";
                } else if ([radioTech isEqualToString:CTRadioAccessTechnologyWCDMA] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyHSDPA] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyHSUPA] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyCDMA1x] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyCDMAEVDORev0] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyCDMAEVDORevA] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyCDMAEVDORevB] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyeHRPD] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyGPRS] ||
                          [radioTech isEqualToString:CTRadioAccessTechnologyEdge]) {
                    connection_type = "cellular";
                }
            }
        }
        
        if (connection_type == "unknown") {
            connection_type = "wifi";
        }
    }
    
    return connection_type;
}

std::vector<std::string> iOSNativeAPI::enumerate_media_devices_ios() {
    std::vector<std::string> devices;
    
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSArray<AVAudioSessionPortDescription*>* inputs = [session availableInputs];
        
        for (AVAudioSessionPortDescription* input in inputs) {
            std::string deviceName = std::string([[input portName] UTF8String]);
            devices.push_back("audioinput:" + deviceName);
        }
        
        NSArray<AVCaptureDevice*>* videoDevices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
        for (AVCaptureDevice* device in videoDevices) {
            std::string deviceName = std::string([[device localizedName] UTF8String]);
            devices.push_back("videoinput:" + deviceName);
        }
        
        devices.push_back("audiooutput:Default Audio Output");
    }
    
    return devices;
}

}

#endif
