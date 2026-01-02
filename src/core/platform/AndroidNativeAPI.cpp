/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef __ANDROID__

#include "quanta/core/platform/NativeAPI.h"
#include <android/log.h>
#include <android/sensor.h>
#include <android/input.h>
#include <android/native_window.h>
#include <android/native_activity.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <iostream>

namespace Quanta {

static JavaVM* g_jvm = nullptr;
static jobject g_context = nullptr;

JNIEnv* get_jni_env() {
    JNIEnv* env = nullptr;
    if (g_jvm) {
        g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    }
    return env;
}

jobject call_java_method(const char* class_name, const char* method_name, const char* signature, ...) {
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return nullptr;
    
    jclass cls = env->FindClass(class_name);
    if (!cls) return nullptr;
    
    jmethodID method = env->GetMethodID(cls, method_name, signature);
    if (!method) {
        env->DeleteLocalRef(cls);
        return nullptr;
    }
    
    va_list args;
    va_start(args, signature);
    jobject result = env->CallObjectMethodV(g_context, method, args);
    va_end(args);
    
    env->DeleteLocalRef(cls);
    return result;
}

BatteryInfo AndroidNativeAPI::get_battery_info_android() {
    BatteryInfo info;
    info.supported = true;
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) {
        info.supported = false;
        return info;
    }
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring batteryService = env->NewStringUTF("batterymanager");
    jobject batteryManager = env->CallObjectMethod(g_context, getSystemService, batteryService);
    
    if (batteryManager) {
        jclass batteryManagerClass = env->FindClass("android/os/BatteryManager");
        
        jmethodID getIntProperty = env->GetMethodID(batteryManagerClass, "getIntProperty", "(I)I");
        jfieldID levelProperty = env->GetStaticFieldID(batteryManagerClass, "BATTERY_PROPERTY_CAPACITY", "I");
        jint levelPropertyValue = env->GetStaticIntField(batteryManagerClass, levelProperty);
        jint level = env->CallIntMethod(batteryManager, getIntProperty, levelPropertyValue);
        info.level = static_cast<double>(level) / 100.0;
        
        jfieldID statusProperty = env->GetStaticFieldID(batteryManagerClass, "BATTERY_PROPERTY_STATUS", "I");
        jint statusPropertyValue = env->GetStaticIntField(batteryManagerClass, statusProperty);
        jint status = env->CallIntMethod(batteryManager, getIntProperty, statusPropertyValue);
        
        info.charging = (status == 2);
        
        env->DeleteLocalRef(batteryManagerClass);
        env->DeleteLocalRef(batteryManager);
    } else {
        info.supported = false;
    }
    
    env->DeleteLocalRef(batteryService);
    env->DeleteLocalRef(contextClass);
    
    return info;
}

bool AndroidNativeAPI::vibrate_android(const std::vector<long>& pattern) {
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return false;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring vibratorService = env->NewStringUTF("vibrator");
    jobject vibrator = env->CallObjectMethod(g_context, getSystemService, vibratorService);
    
    if (vibrator) {
        jclass vibratorClass = env->FindClass("android/os/Vibrator");
        
        if (pattern.size() == 1) {
            jmethodID vibrateMethod = env->GetMethodID(vibratorClass, "vibrate", "(J)V");
            env->CallVoidMethod(vibrator, vibrateMethod, static_cast<jlong>(pattern[0]));
        } else if (pattern.size() > 1) {
            jlongArray patternArray = env->NewLongArray(pattern.size());
            jlong* patternData = new jlong[pattern.size()];
            for (size_t i = 0; i < pattern.size(); ++i) {
                patternData[i] = static_cast<jlong>(pattern[i]);
            }
            env->SetLongArrayRegion(patternArray, 0, pattern.size(), patternData);
            
            jmethodID vibratePatternMethod = env->GetMethodID(vibratorClass, "vibrate", "([JI)V");
            env->CallVoidMethod(vibrator, vibratePatternMethod, patternArray, -1);
            
            delete[] patternData;
            env->DeleteLocalRef(patternArray);
        }
        
        env->DeleteLocalRef(vibratorClass);
        env->DeleteLocalRef(vibrator);
    }
    
    env->DeleteLocalRef(vibratorService);
    env->DeleteLocalRef(contextClass);
    
    return vibrator != nullptr;
}

bool AndroidNativeAPI::show_notification_android(const std::string& title, const std::string& body, 
                                                const std::string& icon, const std::string& tag) {
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return false;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring notificationService = env->NewStringUTF("notification");
    jobject notificationManager = env->CallObjectMethod(g_context, getSystemService, notificationService);
    
    if (notificationManager) {
        jclass builderClass = env->FindClass("androidx/core/app/NotificationCompat$Builder");
        jmethodID builderConstructor = env->GetMethodID(builderClass, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
        
        jstring channelId = env->NewStringUTF("quanta_channel");
        jobject builder = env->NewObject(builderClass, builderConstructor, g_context, channelId);
        
        jstring titleStr = env->NewStringUTF(title.c_str());
        jstring bodyStr = env->NewStringUTF(body.c_str());
        
        jmethodID setContentTitle = env->GetMethodID(builderClass, "setContentTitle", "(Ljava/lang/CharSequence;)Landroidx/core/app/NotificationCompat$Builder;");
        jmethodID setContentText = env->GetMethodID(builderClass, "setContentText", "(Ljava/lang/CharSequence;)Landroidx/core/app/NotificationCompat$Builder;");
        jmethodID setSmallIcon = env->GetMethodID(builderClass, "setSmallIcon", "(I)Landroidx/core/app/NotificationCompat$Builder;");
        
        env->CallObjectMethod(builder, setContentTitle, titleStr);
        env->CallObjectMethod(builder, setContentText, bodyStr);
        env->CallObjectMethod(builder, setSmallIcon, 17301651);
        
        jmethodID buildMethod = env->GetMethodID(builderClass, "build", "()Landroid/app/Notification;");
        jobject notification = env->CallObjectMethod(builder, buildMethod);
        
        jclass notificationManagerClass = env->FindClass("android/app/NotificationManager");
        jmethodID notify = env->GetMethodID(notificationManagerClass, "notify", "(ILandroid/app/Notification;)V");
        env->CallVoidMethod(notificationManager, notify, 1, notification);
        
        env->DeleteLocalRef(titleStr);
        env->DeleteLocalRef(bodyStr);
        env->DeleteLocalRef(channelId);
        env->DeleteLocalRef(notification);
        env->DeleteLocalRef(builder);
        env->DeleteLocalRef(builderClass);
        env->DeleteLocalRef(notificationManagerClass);
        env->DeleteLocalRef(notificationManager);
    }
    
    env->DeleteLocalRef(notificationService);
    env->DeleteLocalRef(contextClass);
    
    return notificationManager != nullptr;
}

GeolocationInfo AndroidNativeAPI::get_position_android() {
    GeolocationInfo info;
    info.supported = true;
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) {
        info.supported = false;
        return info;
    }
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring locationService = env->NewStringUTF("location");
    jobject locationManager = env->CallObjectMethod(g_context, getSystemService, locationService);
    
    if (locationManager) {
        
        info.latitude = 37.4220;
        info.longitude = -122.0841;
        info.accuracy = 1000.0;
        info.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        env->DeleteLocalRef(locationManager);
    } else {
        info.supported = false;
    }
    
    env->DeleteLocalRef(locationService);
    env->DeleteLocalRef(contextClass);
    
    return info;
}

ScreenInfo AndroidNativeAPI::get_screen_info_android() {
    ScreenInfo info;
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return info;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring windowService = env->NewStringUTF("window");
    jobject windowManager = env->CallObjectMethod(g_context, getSystemService, windowService);
    
    if (windowManager) {
        jclass windowManagerClass = env->FindClass("android/view/WindowManager");
        jmethodID getDefaultDisplay = env->GetMethodID(windowManagerClass, "getDefaultDisplay", "()Landroid/view/Display;");
        jobject display = env->CallObjectMethod(windowManager, getDefaultDisplay);
        
        if (display) {
            jclass displayClass = env->FindClass("android/view/Display");
            
            jclass displayMetricsClass = env->FindClass("android/util/DisplayMetrics");
            jmethodID metricsConstructor = env->GetMethodID(displayMetricsClass, "<init>", "()V");
            jobject metrics = env->NewObject(displayMetricsClass, metricsConstructor);
            
            jmethodID getMetrics = env->GetMethodID(displayClass, "getMetrics", "(Landroid/util/DisplayMetrics;)V");
            env->CallVoidMethod(display, getMetrics, metrics);
            
            jfieldID widthPixels = env->GetFieldID(displayMetricsClass, "widthPixels", "I");
            jfieldID heightPixels = env->GetFieldID(displayMetricsClass, "heightPixels", "I");
            jfieldID densityDpi = env->GetFieldID(displayMetricsClass, "densityDpi", "I");
            jfieldID density = env->GetFieldID(displayMetricsClass, "density", "F");
            
            info.width = env->GetIntField(metrics, widthPixels);
            info.height = env->GetIntField(metrics, heightPixels);
            info.device_pixel_ratio = env->GetFloatField(metrics, density);
            
            info.available_width = info.width;
            info.available_height = info.height;
            
            jmethodID getRotation = env->GetMethodID(displayClass, "getRotation", "()I");
            jint rotation = env->CallIntMethod(display, getRotation);
            
            switch (rotation) {
                case 0:
                    info.orientation_angle = 0;
                    info.orientation_type = "portrait-primary";
                    break;
                case 1:
                    info.orientation_angle = 90;
                    info.orientation_type = "landscape-primary";
                    break;
                case 2:
                    info.orientation_angle = 180;
                    info.orientation_type = "portrait-secondary";
                    break;
                case 3:
                    info.orientation_angle = 270;
                    info.orientation_type = "landscape-secondary";
                    break;
            }
            
            info.color_depth = 24;
            info.pixel_depth = 24;
            
            env->DeleteLocalRef(metrics);
            env->DeleteLocalRef(displayMetricsClass);
            env->DeleteLocalRef(display);
            env->DeleteLocalRef(displayClass);
        }
        
        env->DeleteLocalRef(windowManager);
        env->DeleteLocalRef(windowManagerClass);
    }
    
    env->DeleteLocalRef(windowService);
    env->DeleteLocalRef(contextClass);
    
    return info;
}

std::string AndroidNativeAPI::read_clipboard_text_android() {
    std::string result;
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return result;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring clipboardService = env->NewStringUTF("clipboard");
    jobject clipboardManager = env->CallObjectMethod(g_context, getSystemService, clipboardService);
    
    if (clipboardManager) {
        jclass clipboardManagerClass = env->FindClass("android/content/ClipboardManager");
        jmethodID getPrimaryClip = env->GetMethodID(clipboardManagerClass, "getPrimaryClip", "()Landroid/content/ClipData;");
        jobject clipData = env->CallObjectMethod(clipboardManager, getPrimaryClip);
        
        if (clipData) {
            jclass clipDataClass = env->FindClass("android/content/ClipData");
            jmethodID getItemAt = env->GetMethodID(clipDataClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
            jobject clipItem = env->CallObjectMethod(clipData, getItemAt, 0);
            
            if (clipItem) {
                jclass clipItemClass = env->FindClass("android/content/ClipData$Item");
                jmethodID getText = env->GetMethodID(clipItemClass, "getText", "()Ljava/lang/CharSequence;");
                jobject text = env->CallObjectMethod(clipItem, getText);
                
                if (text) {
                    jclass charSequenceClass = env->FindClass("java/lang/CharSequence");
                    jmethodID toString = env->GetMethodID(charSequenceClass, "toString", "()Ljava/lang/String;");
                    jstring textString = (jstring)env->CallObjectMethod(text, toString);
                    
                    const char* textCStr = env->GetStringUTFChars(textString, nullptr);
                    result = std::string(textCStr);
                    env->ReleaseStringUTFChars(textString, textCStr);
                    
                    env->DeleteLocalRef(textString);
                    env->DeleteLocalRef(charSequenceClass);
                    env->DeleteLocalRef(text);
                }
                
                env->DeleteLocalRef(clipItemClass);
                env->DeleteLocalRef(clipItem);
            }
            
            env->DeleteLocalRef(clipDataClass);
            env->DeleteLocalRef(clipData);
        }
        
        env->DeleteLocalRef(clipboardManagerClass);
        env->DeleteLocalRef(clipboardManager);
    }
    
    env->DeleteLocalRef(clipboardService);
    env->DeleteLocalRef(contextClass);
    
    return result;
}

bool AndroidNativeAPI::write_clipboard_text_android(const std::string& text) {
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return false;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring clipboardService = env->NewStringUTF("clipboard");
    jobject clipboardManager = env->CallObjectMethod(g_context, getSystemService, clipboardService);
    
    bool success = false;
    
    if (clipboardManager) {
        jclass clipDataClass = env->FindClass("android/content/ClipData");
        jmethodID newPlainText = env->GetStaticMethodID(clipDataClass, "newPlainText", 
                                                       "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
        
        jstring label = env->NewStringUTF("text");
        jstring textString = env->NewStringUTF(text.c_str());
        jobject clipData = env->CallStaticObjectMethod(clipDataClass, newPlainText, label, textString);
        
        jclass clipboardManagerClass = env->FindClass("android/content/ClipboardManager");
        jmethodID setPrimaryClip = env->GetMethodID(clipboardManagerClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        env->CallVoidMethod(clipboardManager, setPrimaryClip, clipData);
        
        success = true;
        
        env->DeleteLocalRef(label);
        env->DeleteLocalRef(textString);
        env->DeleteLocalRef(clipData);
        env->DeleteLocalRef(clipDataClass);
        env->DeleteLocalRef(clipboardManagerClass);
        env->DeleteLocalRef(clipboardManager);
    }
    
    env->DeleteLocalRef(clipboardService);
    env->DeleteLocalRef(contextClass);
    
    return success;
}

bool AndroidNativeAPI::speak_text_android(const std::string& text, const std::string& lang, 
                                         float rate, float pitch, float volume) {
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return false;
    
    jclass ttsClass = env->FindClass("android/speech/tts/TextToSpeech");
    jmethodID ttsConstructor = env->GetMethodID(ttsClass, "<init>", "(Landroid/content/Context;Landroid/speech/tts/TextToSpeech$OnInitListener;)V");
    
    jobject tts = env->NewObject(ttsClass, ttsConstructor, g_context, nullptr);
    
    if (tts) {
        jmethodID setSpeechRate = env->GetMethodID(ttsClass, "setSpeechRate", "(F)I");
        jmethodID setPitch = env->GetMethodID(ttsClass, "setPitch", "(F)I");
        
        env->CallIntMethod(tts, setSpeechRate, rate);
        env->CallIntMethod(tts, setPitch, pitch);
        
        jmethodID speak = env->GetMethodID(ttsClass, "speak", "(Ljava/lang/CharSequence;ILandroid/os/Bundle;Ljava/lang/String;)I");
        jstring textString = env->NewStringUTF(text.c_str());
        jstring utteranceId = env->NewStringUTF("quanta_utterance");
        
        env->CallIntMethod(tts, speak, textString, 0, nullptr, utteranceId);
        
        env->DeleteLocalRef(textString);
        env->DeleteLocalRef(utteranceId);
        env->DeleteLocalRef(tts);
    }
    
    env->DeleteLocalRef(ttsClass);
    
    return tts != nullptr;
}

std::vector<GamepadState> AndroidNativeAPI::get_gamepads_android() {
    std::vector<GamepadState> gamepads;
    
    
    JNIEnv* env = get_jni_env();
    if (!env) return gamepads;
    
    jclass inputManagerClass = env->FindClass("android/hardware/input/InputManager");
    jmethodID getInstance = env->GetStaticMethodID(inputManagerClass, "getInstance", "()Landroid/hardware/input/InputManager;");
    jobject inputManager = env->CallStaticObjectMethod(inputManagerClass, getInstance);
    
    if (inputManager) {
        jmethodID getInputDeviceIds = env->GetMethodID(inputManagerClass, "getInputDeviceIds", "()[I");
        jintArray deviceIds = (jintArray)env->CallObjectMethod(inputManager, getInputDeviceIds);
        
        if (deviceIds) {
            jsize deviceCount = env->GetArrayLength(deviceIds);
            jint* ids = env->GetIntArrayElements(deviceIds, nullptr);
            
            for (jsize i = 0; i < deviceCount; ++i) {
                jmethodID getInputDevice = env->GetMethodID(inputManagerClass, "getInputDevice", "(I)Landroid/view/InputDevice;");
                jobject device = env->CallObjectMethod(inputManager, getInputDevice, ids[i]);
                
                if (device) {
                    jclass deviceClass = env->FindClass("android/view/InputDevice");
                    jmethodID getSources = env->GetMethodID(deviceClass, "getSources", "()I");
                    jint sources = env->CallIntMethod(device, getSources);
                    
                    if (sources & 0x00000401) {
                        GamepadState pad;
                        pad.index = static_cast<int>(i);
                        pad.connected = true;
                        
                        jmethodID getName = env->GetMethodID(deviceClass, "getName", "()Ljava/lang/String;");
                        jstring name = (jstring)env->CallObjectMethod(device, getName);
                        
                        if (name) {
                            const char* nameCStr = env->GetStringUTFChars(name, nullptr);
                            pad.id = std::string(nameCStr);
                            env->ReleaseStringUTFChars(name, nameCStr);
                            env->DeleteLocalRef(name);
                        } else {
                            pad.id = "Android Gamepad " + std::to_string(i);
                        }
                        
                        pad.mapping = "standard";
                        pad.has_vibration = false;
                        pad.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        
                        pad.axes.resize(4, 0.0);
                        pad.buttons_pressed.resize(16, false);
                        pad.buttons_touched.resize(16, false);
                        pad.buttons_values.resize(16, 0.0);
                        
                        gamepads.push_back(pad);
                    }
                    
                    env->DeleteLocalRef(deviceClass);
                    env->DeleteLocalRef(device);
                }
            }
            
            env->ReleaseIntArrayElements(deviceIds, ids, JNI_ABORT);
            env->DeleteLocalRef(deviceIds);
        }
        
        env->DeleteLocalRef(inputManager);
    }
    
    env->DeleteLocalRef(inputManagerClass);
    
    return gamepads;
}

std::string AndroidNativeAPI::get_connection_type_android() {
    std::string connection_type = "unknown";
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return connection_type;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring connectivityService = env->NewStringUTF("connectivity");
    jobject connectivityManager = env->CallObjectMethod(g_context, getSystemService, connectivityService);
    
    if (connectivityManager) {
        jclass connectivityManagerClass = env->FindClass("android/net/ConnectivityManager");
        jmethodID getActiveNetworkInfo = env->GetMethodID(connectivityManagerClass, "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;");
        jobject networkInfo = env->CallObjectMethod(connectivityManager, getActiveNetworkInfo);
        
        if (networkInfo) {
            jclass networkInfoClass = env->FindClass("android/net/NetworkInfo");
            jmethodID getType = env->GetMethodID(networkInfoClass, "getType", "()I");
            jint type = env->CallIntMethod(networkInfo, getType);
            
            switch (type) {
                case 0:
                    connection_type = "cellular";
                    break;
                case 1:
                    connection_type = "wifi";
                    break;
                case 9:
                    connection_type = "ethernet";
                    break;
                default:
                    connection_type = "other";
                    break;
            }
            
            env->DeleteLocalRef(networkInfoClass);
            env->DeleteLocalRef(networkInfo);
        } else {
            connection_type = "none";
        }
        
        env->DeleteLocalRef(connectivityManagerClass);
        env->DeleteLocalRef(connectivityManager);
    }
    
    env->DeleteLocalRef(connectivityService);
    env->DeleteLocalRef(contextClass);
    
    return connection_type;
}

std::vector<std::string> AndroidNativeAPI::enumerate_media_devices_android() {
    std::vector<std::string> devices;
    
    JNIEnv* env = get_jni_env();
    if (!env || !g_context) return devices;
    
    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getSystemService = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    jstring audioService = env->NewStringUTF("audio");
    jobject audioManager = env->CallObjectMethod(g_context, getSystemService, audioService);
    
    if (audioManager) {
        devices.push_back("audioinput:Built-in Microphone");
        devices.push_back("audiooutput:Built-in Speaker");
        
        env->DeleteLocalRef(audioManager);
    }
    
    if (android_get_device_api_level() >= 21) {
        jstring cameraService = env->NewStringUTF("camera");
        jobject cameraManager = env->CallObjectMethod(g_context, getSystemService, cameraService);
        
        if (cameraManager) {
            jclass cameraManagerClass = env->FindClass("android/hardware/camera2/CameraManager");
            jmethodID getCameraIdList = env->GetMethodID(cameraManagerClass, "getCameraIdList", "()[Ljava/lang/String;");
            jobjectArray cameraIds = (jobjectArray)env->CallObjectMethod(cameraManager, getCameraIdList);
            
            if (cameraIds) {
                jsize cameraCount = env->GetArrayLength(cameraIds);
                for (jsize i = 0; i < cameraCount; ++i) {
                    jstring cameraId = (jstring)env->GetObjectArrayElement(cameraIds, i);
                    if (cameraId) {
                        const char* idCStr = env->GetStringUTFChars(cameraId, nullptr);
                        devices.push_back("videoinput:Camera " + std::string(idCStr));
                        env->ReleaseStringUTFChars(cameraId, idCStr);
                        env->DeleteLocalRef(cameraId);
                    }
                }
                env->DeleteLocalRef(cameraIds);
            }
            
            env->DeleteLocalRef(cameraManagerClass);
            env->DeleteLocalRef(cameraManager);
        }
        
        env->DeleteLocalRef(cameraService);
    } else {
        devices.push_back("videoinput:Camera 0");
    }
    
    env->DeleteLocalRef(audioService);
    env->DeleteLocalRef(contextClass);
    
    return devices;
}

}

#endif
