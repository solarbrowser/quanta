/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WEBAPI_H
#define QUANTA_WEBAPI_H

#include "Value.h"
#include "Context.h"
#include <chrono>
#include <thread>
#include <functional>
#include <vector>
#include <memory>

namespace Quanta {

// Forward declarations
struct CanvasState;
struct CairoCanvasState;
struct OpenGLWebGLState;

/**
 * Web API implementations
 * Provides browser-like functionality for JavaScript
 */
class WebAPI {
public:
    // Timer APIs
    static Value setTimeout(Context& ctx, const std::vector<Value>& args);
    static Value setInterval(Context& ctx, const std::vector<Value>& args);
    static Value clearTimeout(Context& ctx, const std::vector<Value>& args);
    static Value clearInterval(Context& ctx, const std::vector<Value>& args);
    
    // Console API (enhanced)
    static Value console_log(Context& ctx, const std::vector<Value>& args);
    static Value console_error(Context& ctx, const std::vector<Value>& args);
    static Value console_warn(Context& ctx, const std::vector<Value>& args);
    static Value console_info(Context& ctx, const std::vector<Value>& args);
    static Value console_debug(Context& ctx, const std::vector<Value>& args);
    static Value console_trace(Context& ctx, const std::vector<Value>& args);
    static Value console_time(Context& ctx, const std::vector<Value>& args);
    static Value console_timeEnd(Context& ctx, const std::vector<Value>& args);
    
    // Complete Fetch API Implementation
    static Value fetch(Context& ctx, const std::vector<Value>& args);
    static Value Headers_constructor(Context& ctx, const std::vector<Value>& args);
    static Value Headers_append(Context& ctx, const std::vector<Value>& args);
    static Value Headers_delete(Context& ctx, const std::vector<Value>& args);
    static Value Headers_get(Context& ctx, const std::vector<Value>& args);
    static Value Headers_has(Context& ctx, const std::vector<Value>& args);
    static Value Headers_set(Context& ctx, const std::vector<Value>& args);
    static Value Headers_forEach(Context& ctx, const std::vector<Value>& args);
    static Value Request_constructor(Context& ctx, const std::vector<Value>& args);
    static Value Response_constructor(Context& ctx, const std::vector<Value>& args);
    static Value Response_json(Context& ctx, const std::vector<Value>& args);
    static Value Response_text(Context& ctx, const std::vector<Value>& args);
    static Value Response_blob(Context& ctx, const std::vector<Value>& args);
    static Value Response_arrayBuffer(Context& ctx, const std::vector<Value>& args);
    static Value Response_ok(Context& ctx, const std::vector<Value>& args);
    static Value Response_status(Context& ctx, const std::vector<Value>& args);
    static Value Response_statusText(Context& ctx, const std::vector<Value>& args);
    static Value Response_headers(Context& ctx, const std::vector<Value>& args);
    
    // Complete URL API
    static Value URL_constructor(Context& ctx, const std::vector<Value>& args);
    static Value URL_toString(Context& ctx, const std::vector<Value>& args);
    static Value URL_toJSON(Context& ctx, const std::vector<Value>& args);
    
    // URLSearchParams API  
    static Value URLSearchParams_constructor(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_append(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_delete(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_get(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_getAll(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_has(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_set(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_sort(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_toString(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_forEach(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_keys(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_values(Context& ctx, const std::vector<Value>& args);
    static Value URLSearchParams_entries(Context& ctx, const std::vector<Value>& args);
    
    // Basic DOM API
    static Value document_getElementById(Context& ctx, const std::vector<Value>& args);
    static Value document_getBody(Context& ctx, const std::vector<Value>& args);
    static Value document_createElement(Context& ctx, const std::vector<Value>& args);
    static Value document_querySelector(Context& ctx, const std::vector<Value>& args);
    static Value document_querySelectorAll(Context& ctx, const std::vector<Value>& args);
    static Value document_getElementsByTagName(Context& ctx, const std::vector<Value>& args);
    static Value document_getElementsByClassName(Context& ctx, const std::vector<Value>& args);
    static Value create_dom_element(const std::string& tagName, const std::string& id);
    
    // Window API
    static Value window_alert(Context& ctx, const std::vector<Value>& args);
    static Value window_confirm(Context& ctx, const std::vector<Value>& args);
    static Value window_prompt(Context& ctx, const std::vector<Value>& args);
    
    // Storage API - Basic operations
    static Value localStorage_getItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_setItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_removeItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_clear(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_key(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_length(Context& ctx, const std::vector<Value>& args);
    
    // SessionStorage API - Same interface as localStorage
    static Value sessionStorage_getItem(Context& ctx, const std::vector<Value>& args);
    static Value sessionStorage_setItem(Context& ctx, const std::vector<Value>& args);
    static Value sessionStorage_removeItem(Context& ctx, const std::vector<Value>& args);
    static Value sessionStorage_clear(Context& ctx, const std::vector<Value>& args);
    static Value sessionStorage_key(Context& ctx, const std::vector<Value>& args);
    static Value sessionStorage_length(Context& ctx, const std::vector<Value>& args);
    
    // Navigator Storage API - Modern storage management
    static Value navigator_storage_estimate(Context& ctx, const std::vector<Value>& args);
    static Value navigator_storage_persist(Context& ctx, const std::vector<Value>& args);
    static Value navigator_storage_persisted(Context& ctx, const std::vector<Value>& args);
    
    // Storage Events
    static Value storage_addEventListener(Context& ctx, const std::vector<Value>& args);
    static Value storage_dispatchEvent(Context& ctx, const std::vector<Value>& args);
    
    // Cookie API
    static Value document_getCookie(Context& ctx, const std::vector<Value>& args);
    static Value document_setCookie(Context& ctx, const std::vector<Value>& args);
    
    // Complete Crypto API
    static Value crypto_randomUUID(Context& ctx, const std::vector<Value>& args);
    static Value crypto_getRandomValues(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_digest(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_encrypt(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_decrypt(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_generateKey(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_importKey(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_exportKey(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_sign(Context& ctx, const std::vector<Value>& args);
    static Value crypto_subtle_verify(Context& ctx, const std::vector<Value>& args);
    
    static Value FormData_constructor(Context& ctx, const std::vector<Value>& args);
    static Value FormData_append(Context& ctx, const std::vector<Value>& args);
    static Value FormData_delete(Context& ctx, const std::vector<Value>& args);
    static Value FormData_get(Context& ctx, const std::vector<Value>& args);
    static Value FormData_getAll(Context& ctx, const std::vector<Value>& args);
    static Value FormData_has(Context& ctx, const std::vector<Value>& args);
    static Value FormData_set(Context& ctx, const std::vector<Value>& args);
    static Value FormData_keys(Context& ctx, const std::vector<Value>& args);
    static Value FormData_values(Context& ctx, const std::vector<Value>& args);
    static Value FormData_entries(Context& ctx, const std::vector<Value>& args);
    static Value FormData_forEach(Context& ctx, const std::vector<Value>& args);
    
    // Complete Media APIs
    static Value MediaStream_constructor(Context& ctx, const std::vector<Value>& args);
    static Value MediaStream_getTracks(Context& ctx, const std::vector<Value>& args);
    static Value MediaStream_getAudioTracks(Context& ctx, const std::vector<Value>& args);
    static Value media_element_play(Context& ctx, const std::vector<Value>& args);
    static Value media_element_pause(Context& ctx, const std::vector<Value>& args);
    static Value media_element_load(Context& ctx, const std::vector<Value>& args);
    
    // Geolocation API
    static Value navigator_geolocation_getCurrentPosition(Context& ctx, const std::vector<Value>& args);
    static Value navigator_geolocation_watchPosition(Context& ctx, const std::vector<Value>& args);
    static Value navigator_geolocation_clearWatch(Context& ctx, const std::vector<Value>& args);
    
    // Notification API
    static Value Notification_constructor(Context& ctx, const std::vector<Value>& args);
    static Value Notification_requestPermission(Context& ctx, const std::vector<Value>& args);
    static Value Notification_close(Context& ctx, const std::vector<Value>& args);
    static Value notification_click(Context& ctx, const std::vector<Value>& args);
    static Value notification_show(Context& ctx, const std::vector<Value>& args);
    static Value notification_error(Context& ctx, const std::vector<Value>& args);
    
    // Complete History API
    static Value history_pushState(Context& ctx, const std::vector<Value>& args);
    static Value history_replaceState(Context& ctx, const std::vector<Value>& args);
    static Value history_back(Context& ctx, const std::vector<Value>& args);
    static Value history_forward(Context& ctx, const std::vector<Value>& args);
    static Value history_go(Context& ctx, const std::vector<Value>& args);
    static Value history_length(Context& ctx, const std::vector<Value>& args);
    static Value history_state(Context& ctx, const std::vector<Value>& args);
    static Value history_scrollRestoration(Context& ctx, const std::vector<Value>& args);
    
    // Complete Location API
    static Value location_href(Context& ctx, const std::vector<Value>& args);
    static Value location_protocol(Context& ctx, const std::vector<Value>& args);
    static Value location_host(Context& ctx, const std::vector<Value>& args);
    static Value location_hostname(Context& ctx, const std::vector<Value>& args);
    static Value location_port(Context& ctx, const std::vector<Value>& args);
    static Value location_pathname(Context& ctx, const std::vector<Value>& args);
    static Value location_search(Context& ctx, const std::vector<Value>& args);
    static Value location_hash(Context& ctx, const std::vector<Value>& args);
    static Value location_origin(Context& ctx, const std::vector<Value>& args);
    static Value location_assign(Context& ctx, const std::vector<Value>& args);
    static Value location_replace(Context& ctx, const std::vector<Value>& args);
    static Value location_reload(Context& ctx, const std::vector<Value>& args);
    static Value location_toString(Context& ctx, const std::vector<Value>& args);
    
    // Complete Performance API
    static Value performance_now(Context& ctx, const std::vector<Value>& args);
    static Value performance_mark(Context& ctx, const std::vector<Value>& args);
    static Value performance_measure(Context& ctx, const std::vector<Value>& args);
    static Value performance_clearMarks(Context& ctx, const std::vector<Value>& args);
    static Value performance_clearMeasures(Context& ctx, const std::vector<Value>& args);
    static Value performance_getEntries(Context& ctx, const std::vector<Value>& args);
    static Value performance_getEntriesByName(Context& ctx, const std::vector<Value>& args);
    static Value performance_getEntriesByType(Context& ctx, const std::vector<Value>& args);
    
    // Complete Clipboard API
    static Value navigator_clipboard_read(Context& ctx, const std::vector<Value>& args);
    static Value navigator_clipboard_readText(Context& ctx, const std::vector<Value>& args);
    static Value navigator_clipboard_write(Context& ctx, const std::vector<Value>& args);
    static Value navigator_clipboard_writeText(Context& ctx, const std::vector<Value>& args);
    
    // Battery API
    static Value navigator_getBattery(Context& ctx, const std::vector<Value>& args);
    static Value battery_charging(Context& ctx, const std::vector<Value>& args);
    static Value battery_chargingTime(Context& ctx, const std::vector<Value>& args);
    static Value battery_dischargingTime(Context& ctx, const std::vector<Value>& args);
    static Value battery_level(Context& ctx, const std::vector<Value>& args);
    
    // Network Information API
    static Value navigator_connection_type(Context& ctx, const std::vector<Value>& args);
    static Value navigator_connection_effectiveType(Context& ctx, const std::vector<Value>& args);
    static Value navigator_connection_downlink(Context& ctx, const std::vector<Value>& args);
    static Value navigator_connection_uplink(Context& ctx, const std::vector<Value>& args);
    static Value navigator_connection_rtt(Context& ctx, const std::vector<Value>& args);
    static Value navigator_connection_saveData(Context& ctx, const std::vector<Value>& args);
    static Value navigator_onLine(Context& ctx, const std::vector<Value>& args);
    
    // Vibration API
    static Value navigator_vibrate(Context& ctx, const std::vector<Value>& args);
    
    // Device Orientation API
    static Value window_addEventListener_deviceorientation(Context& ctx, const std::vector<Value>& args);
    static Value window_addEventListener_devicemotion(Context& ctx, const std::vector<Value>& args);
    static Value deviceOrientationEvent_alpha(Context& ctx, const std::vector<Value>& args);
    static Value deviceOrientationEvent_beta(Context& ctx, const std::vector<Value>& args);
    static Value deviceOrientationEvent_gamma(Context& ctx, const std::vector<Value>& args);
    static Value deviceOrientationEvent_absolute(Context& ctx, const std::vector<Value>& args);
    static Value deviceMotionEvent_acceleration(Context& ctx, const std::vector<Value>& args);
    static Value deviceMotionEvent_accelerationIncludingGravity(Context& ctx, const std::vector<Value>& args);
    static Value deviceMotionEvent_rotationRate(Context& ctx, const std::vector<Value>& args);
    static Value deviceMotionEvent_interval(Context& ctx, const std::vector<Value>& args);
    
    // Screen API
    static Value screen_width(Context& ctx, const std::vector<Value>& args);
    static Value screen_height(Context& ctx, const std::vector<Value>& args);
    static Value screen_availWidth(Context& ctx, const std::vector<Value>& args);
    static Value screen_availHeight(Context& ctx, const std::vector<Value>& args);
    static Value screen_colorDepth(Context& ctx, const std::vector<Value>& args);
    static Value screen_pixelDepth(Context& ctx, const std::vector<Value>& args);
    static Value screen_orientation_angle(Context& ctx, const std::vector<Value>& args);
    static Value screen_orientation_type(Context& ctx, const std::vector<Value>& args);
    
    // Intersection Observer API
    static Value IntersectionObserver_constructor(Context& ctx, const std::vector<Value>& args);
    static Value IntersectionObserver_observe(Context& ctx, const std::vector<Value>& args);
    static Value IntersectionObserver_unobserve(Context& ctx, const std::vector<Value>& args);
    static Value IntersectionObserver_disconnect(Context& ctx, const std::vector<Value>& args);
    
    // Resize Observer API
    static Value ResizeObserver_constructor(Context& ctx, const std::vector<Value>& args);
    static Value ResizeObserver_observe(Context& ctx, const std::vector<Value>& args);
    static Value ResizeObserver_unobserve(Context& ctx, const std::vector<Value>& args);
    static Value ResizeObserver_disconnect(Context& ctx, const std::vector<Value>& args);
    
    // Audio API
    static Value Audio_constructor(Context& ctx, const std::vector<Value>& args);
    
    // Typed Arrays API
    static Value Uint8Array_constructor(Context& ctx, const std::vector<Value>& args);
    
    // Service Workers API - Background processing and offline capabilities
    static Value navigator_serviceWorker_register(Context& ctx, const std::vector<Value>& args);
    static Value navigator_serviceWorker_getRegistration(Context& ctx, const std::vector<Value>& args);
    static Value navigator_serviceWorker_getRegistrations(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorkerRegistration_update(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorkerRegistration_unregister(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorkerRegistration_showNotification(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorkerRegistration_getNotifications(Context& ctx, const std::vector<Value>& args);
    
    // Cache API - Offline storage for Service Workers
    static Value caches_open(Context& ctx, const std::vector<Value>& args);
    static Value caches_delete(Context& ctx, const std::vector<Value>& args);
    static Value caches_has(Context& ctx, const std::vector<Value>& args);
    static Value caches_keys(Context& ctx, const std::vector<Value>& args);
    static Value caches_match(Context& ctx, const std::vector<Value>& args);
    static Value cache_add(Context& ctx, const std::vector<Value>& args);
    static Value cache_addAll(Context& ctx, const std::vector<Value>& args);
    static Value cache_match(Context& ctx, const std::vector<Value>& args);
    static Value cache_matchAll(Context& ctx, const std::vector<Value>& args);
    static Value cache_put(Context& ctx, const std::vector<Value>& args);
    static Value cache_delete(Context& ctx, const std::vector<Value>& args);
    static Value cache_keys(Context& ctx, const std::vector<Value>& args);
    
    // Service Worker Events
    static Value serviceWorker_install(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_activate(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_fetch(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_push(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_notificationclick(Context& ctx, const std::vector<Value>& args);
    
    // WebSocket API - Real-time bidirectional communication
    static Value WebSocket_constructor(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_send(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_close(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_addEventListener(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_removeEventListener(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_dispatchEvent(Context& ctx, const std::vector<Value>& args);
    
    // WebSocket Event Handlers
    static Value webSocket_onopen(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_onmessage(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_onerror(Context& ctx, const std::vector<Value>& args);
    static Value webSocket_onclose(Context& ctx, const std::vector<Value>& args);
    
    // WebSocket Utility Functions
    static Value create_websocket_event(const std::string& type, const Value& data = Value());
    static Value create_message_event(const Value& data, const std::string& origin = "");
    static Value create_close_event(int code, const std::string& reason, bool wasClean);
    
    // WebRTC API - Peer-to-peer video/audio streaming
    static Value rtcPeerConnection_createDataChannel(Context& ctx, const std::vector<Value>& args);
    static Value rtcDataChannel_send(Context& ctx, const std::vector<Value>& args);
    static Value rtcDataChannel_close(Context& ctx, const std::vector<Value>& args);
    static Value rtcDataChannel_addEventListener(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_addTrack(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_removeTrack(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_getSenders(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_getReceivers(Context& ctx, const std::vector<Value>& args);
    static Value rtcSender_replaceTrack(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_addEventListener(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_removeEventListener(Context& ctx, const std::vector<Value>& args);
    static Value rtcPeerConnection_close(Context& ctx, const std::vector<Value>& args);
    
    // Media Stream and Track APIs
    static Value create_media_track(const std::string& kind);
    static Value mediaTrack_stop(Context& ctx, const std::vector<Value>& args);
    static Value mediaTrack_clone(Context& ctx, const std::vector<Value>& args);
    static Value mediaTrack_getSettings(Context& ctx, const std::vector<Value>& args);
    
    // Event system (basic)
    static Value addEventListener(Context& ctx, const std::vector<Value>& args);
    static Value removeEventListener(Context& ctx, const std::vector<Value>& args);
    static Value dispatchEvent(Context& ctx, const std::vector<Value>& args);
    
    // Canvas 2D Context API
    static Value canvas_getContext(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_fillRect(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_strokeRect(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_clearRect(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_fillText(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_strokeText(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_beginPath(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_moveTo(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_lineTo(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_arc(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_fill(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_stroke(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_setTransform(Context& ctx, const std::vector<Value>& args);
    static Value canvas2d_drawImage(Context& ctx, const std::vector<Value>& args);
    static Value create_canvas_element(int width, int height);
    static Value create_canvas_2d_context();
    static Value create_canvas_2d_context_with_state(CanvasState* canvas_state);
    static Value create_cairo_2d_context(CairoCanvasState* cairo_canvas);
    
    // React Component Lifecycle API
    static Value React_Component_constructor(Context& ctx, const std::vector<Value>& args);
    static Value React_createElement(Context& ctx, const std::vector<Value>& args);
    static Value React_createClass(Context& ctx, const std::vector<Value>& args);
    static Value create_react_component(const std::string& name);
    static Value component_render(Context& ctx, const std::vector<Value>& args);
    static Value component_componentDidMount(Context& ctx, const std::vector<Value>& args);
    static Value component_componentDidUpdate(Context& ctx, const std::vector<Value>& args);
    static Value component_componentWillUnmount(Context& ctx, const std::vector<Value>& args);
    static Value component_setState(Context& ctx, const std::vector<Value>& args);
    static Value component_forceUpdate(Context& ctx, const std::vector<Value>& args);
    
    // Virtual DOM Diffing Algorithm
    static Value ReactDOM_render(Context& ctx, const std::vector<Value>& args);
    static Value vdom_diff(Context& ctx, const std::vector<Value>& args);
    static Value vdom_patch(Context& ctx, const std::vector<Value>& args);
    static Value create_vdom_node(const Value& element);
    static Value diff_elements(const Value& oldElement, const Value& newElement);
    static Value diff_children(const Value& oldChildren, const Value& newChildren);
    static Value apply_patches(const Value& domNode, const Value& patches);
    
    // WebGL Support for 3D Graphics
    static Value canvas_getWebGLContext(Context& ctx, const std::vector<Value>& args);
    static Value create_webgl_context();
    static Value webgl_createShader(Context& ctx, const std::vector<Value>& args);
    static Value webgl_shaderSource(Context& ctx, const std::vector<Value>& args);
    static Value webgl_compileShader(Context& ctx, const std::vector<Value>& args);
    static Value webgl_createProgram(Context& ctx, const std::vector<Value>& args);
    static Value webgl_attachShader(Context& ctx, const std::vector<Value>& args);
    static Value webgl_linkProgram(Context& ctx, const std::vector<Value>& args);
    static Value webgl_useProgram(Context& ctx, const std::vector<Value>& args);
    static Value webgl_createBuffer(Context& ctx, const std::vector<Value>& args);
    static Value webgl_bindBuffer(Context& ctx, const std::vector<Value>& args);
    static Value webgl_bufferData(Context& ctx, const std::vector<Value>& args);
    static Value webgl_getAttribLocation(Context& ctx, const std::vector<Value>& args);
    static Value webgl_enableVertexAttribArray(Context& ctx, const std::vector<Value>& args);
    static Value webgl_vertexAttribPointer(Context& ctx, const std::vector<Value>& args);
    static Value webgl_getUniformLocation(Context& ctx, const std::vector<Value>& args);
    static Value webgl_uniformMatrix4fv(Context& ctx, const std::vector<Value>& args);
    static Value webgl_uniform3fv(Context& ctx, const std::vector<Value>& args);
    static Value webgl_clear(Context& ctx, const std::vector<Value>& args);
    static Value webgl_clearColor(Context& ctx, const std::vector<Value>& args);
    static Value webgl_enable(Context& ctx, const std::vector<Value>& args);
    static Value webgl_viewport(Context& ctx, const std::vector<Value>& args);
    static Value webgl_drawArrays(Context& ctx, const std::vector<Value>& args);
    static Value webgl_drawElements(Context& ctx, const std::vector<Value>& args);
    
    // Web Audio API for Sound Processing
    static Value create_audio_context();
    static Value audio_createOscillator(Context& ctx, const std::vector<Value>& args);
    static Value audio_createGain(Context& ctx, const std::vector<Value>& args);
    static Value audio_createAnalyser(Context& ctx, const std::vector<Value>& args);
    static Value audio_createBuffer(Context& ctx, const std::vector<Value>& args);
    static Value audio_createBufferSource(Context& ctx, const std::vector<Value>& args);
    static Value audio_decodeAudioData(Context& ctx, const std::vector<Value>& args);
    static Value audioNode_connect(Context& ctx, const std::vector<Value>& args);
    static Value audioNode_disconnect(Context& ctx, const std::vector<Value>& args);
    static Value oscillator_start(Context& ctx, const std::vector<Value>& args);
    static Value oscillator_stop(Context& ctx, const std::vector<Value>& args);
    static Value audioParam_setValueAtTime(Context& ctx, const std::vector<Value>& args);
    static Value audioParam_linearRampToValueAtTime(Context& ctx, const std::vector<Value>& args);
    static Value analyserNode_getByteFrequencyData(Context& ctx, const std::vector<Value>& args);
    static Value bufferSource_start(Context& ctx, const std::vector<Value>& args);
    
    // REAL File System API - Node.js-style fs module
    static Value fs_readFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_readFileSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_writeFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_writeFileSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_appendFile(Context& ctx, const std::vector<Value>& args);
    static Value fs_readdir(Context& ctx, const std::vector<Value>& args);
    static Value fs_readdirSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_mkdir(Context& ctx, const std::vector<Value>& args);
    static Value fs_mkdirSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_unlink(Context& ctx, const std::vector<Value>& args);
    static Value fs_unlinkSync(Context& ctx, const std::vector<Value>& args);
    static Value fs_stat(Context& ctx, const std::vector<Value>& args);
    static Value fs_statSync(Context& ctx, const std::vector<Value>& args);
    
    // IndexedDB API - Client-side database with transactions
    static Value indexedDB_open(Context& ctx, const std::vector<Value>& args);
    static Value indexedDB_deleteDatabase(Context& ctx, const std::vector<Value>& args);
    static Value indexedDB_cmp(Context& ctx, const std::vector<Value>& args);
    static Value idbRequest_onsuccess(Context& ctx, const std::vector<Value>& args);
    static Value idbRequest_onerror(Context& ctx, const std::vector<Value>& args);
    static Value idbRequest_onupgradeneeded(Context& ctx, const std::vector<Value>& args);
    static Value idbDatabase_createObjectStore(Context& ctx, const std::vector<Value>& args);
    static Value idbDatabase_deleteObjectStore(Context& ctx, const std::vector<Value>& args);
    static Value idbDatabase_transaction(Context& ctx, const std::vector<Value>& args);
    static Value idbDatabase_close(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_add(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_put(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_get(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_delete(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_clear(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_count(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_createIndex(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_deleteIndex(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_index(Context& ctx, const std::vector<Value>& args);
    static Value idbObjectStore_openCursor(Context& ctx, const std::vector<Value>& args);
    static Value idbTransaction_commit(Context& ctx, const std::vector<Value>& args);
    static Value idbTransaction_abort(Context& ctx, const std::vector<Value>& args);
    static Value idbTransaction_objectStore(Context& ctx, const std::vector<Value>& args);
    static Value idbCursor_continue(Context& ctx, const std::vector<Value>& args);
    static Value idbCursor_update(Context& ctx, const std::vector<Value>& args);
    static Value idbCursor_delete(Context& ctx, const std::vector<Value>& args);
    static Value idbIndex_get(Context& ctx, const std::vector<Value>& args);
    static Value idbIndex_getKey(Context& ctx, const std::vector<Value>& args);
    static Value idbIndex_openCursor(Context& ctx, const std::vector<Value>& args);
    
    // WebRTC API - Real-time peer-to-peer communication
    static Value RTCPeerConnection_constructor(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_createOffer(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_createAnswer(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_setLocalDescription(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_setRemoteDescription(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_addIceCandidate(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_addStream(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_addTrack(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_removeTrack(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_getSenders(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_getReceivers(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_getTransceivers(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_getStats(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_close(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_connectionState(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_iceConnectionState(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_iceGatheringState(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_signalingState(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_localDescription(Context& ctx, const std::vector<Value>& args);
    static Value RTCPeerConnection_remoteDescription(Context& ctx, const std::vector<Value>& args);
    
    // Navigator MediaDevices API - Camera/microphone access
    static Value navigator_mediaDevices_getUserMedia(Context& ctx, const std::vector<Value>& args);
    static Value navigator_mediaDevices_enumerateDevices(Context& ctx, const std::vector<Value>& args);
    static Value navigator_mediaDevices_getDisplayMedia(Context& ctx, const std::vector<Value>& args);
    static Value mediaStream_getTracks(Context& ctx, const std::vector<Value>& args);
    static Value mediaStream_getAudioTracks(Context& ctx, const std::vector<Value>& args);
    static Value mediaStream_getVideoTracks(Context& ctx, const std::vector<Value>& args);
    static Value mediaStream_addTrack(Context& ctx, const std::vector<Value>& args);
    static Value mediaStream_removeTrack(Context& ctx, const std::vector<Value>& args);
    static Value mediaStreamTrack_stop(Context& ctx, const std::vector<Value>& args);
    static Value mediaStreamTrack_enabled(Context& ctx, const std::vector<Value>& args);
    static Value mediaStreamTrack_kind(Context& ctx, const std::vector<Value>& args);
    static Value mediaStreamTrack_label(Context& ctx, const std::vector<Value>& args);
    
    // File API - File system and blob management
    static Value File_constructor(Context& ctx, const std::vector<Value>& args);
    static Value File_name(Context& ctx, const std::vector<Value>& args);
    static Value File_lastModified(Context& ctx, const std::vector<Value>& args);
    static Value File_size(Context& ctx, const std::vector<Value>& args);
    static Value File_type(Context& ctx, const std::vector<Value>& args);
    static Value Blob_constructor(Context& ctx, const std::vector<Value>& args);
    static Value Blob_size(Context& ctx, const std::vector<Value>& args);
    static Value Blob_type(Context& ctx, const std::vector<Value>& args);
    static Value Blob_slice(Context& ctx, const std::vector<Value>& args);
    static Value Blob_stream(Context& ctx, const std::vector<Value>& args);
    static Value Blob_text(Context& ctx, const std::vector<Value>& args);
    static Value Blob_arrayBuffer(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_constructor(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_readAsText(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_readAsDataURL(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_readAsArrayBuffer(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_readAsBinaryString(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_abort(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_result(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_error(Context& ctx, const std::vector<Value>& args);
    static Value FileReader_readyState(Context& ctx, const std::vector<Value>& args);
    
    // Speech Synthesis API - Text-to-speech with real system integration
    static Value speechSynthesis_speak(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_cancel(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_pause(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_resume(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_getVoices(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_speaking(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_pending(Context& ctx, const std::vector<Value>& args);
    static Value speechSynthesis_paused(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_constructor(Context& ctx, const std::vector<Value>& args);
    static Value utterance_text(Context& ctx, const std::vector<Value>& args);
    static Value utterance_lang(Context& ctx, const std::vector<Value>& args);
    static Value utterance_voice(Context& ctx, const std::vector<Value>& args);
    static Value utterance_volume(Context& ctx, const std::vector<Value>& args);
    static Value utterance_rate(Context& ctx, const std::vector<Value>& args);
    static Value utterance_pitch(Context& ctx, const std::vector<Value>& args);
    
    // SpeechSynthesisUtterance property methods (matching Engine.cpp names)
    static Value SpeechSynthesisUtterance_text(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_lang(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_voice(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_volume(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_rate(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisUtterance_pitch(Context& ctx, const std::vector<Value>& args);
    
    // SpeechSynthesisVoice property methods
    static Value SpeechSynthesisVoice_name(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisVoice_lang(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisVoice_default(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisVoice_localService(Context& ctx, const std::vector<Value>& args);
    static Value SpeechSynthesisVoice_voiceURI(Context& ctx, const std::vector<Value>& args);
    
    // Speech Recognition API - Voice-to-text with real system integration
    static Value SpeechRecognition_constructor(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_start(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_stop(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_abort(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_lang(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_continuous(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_interimResults(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_maxAlternatives(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_serviceURI(Context& ctx, const std::vector<Value>& args);
    static Value speechRecognition_grammars(Context& ctx, const std::vector<Value>& args);
    
    // SpeechRecognitionResult methods
    static Value SpeechRecognitionResult_length(Context& ctx, const std::vector<Value>& args);
    static Value SpeechRecognitionResult_item(Context& ctx, const std::vector<Value>& args);
    static Value SpeechRecognitionResult_isFinal(Context& ctx, const std::vector<Value>& args);
    
    // SpeechRecognitionAlternative methods
    static Value SpeechRecognitionAlternative_transcript(Context& ctx, const std::vector<Value>& args);
    static Value SpeechRecognitionAlternative_confidence(Context& ctx, const std::vector<Value>& args);
    
    // Gamepad API - Real controller/joystick support with system integration
    static Value navigator_getGamepads(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_id(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_index(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_connected(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_timestamp(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_mapping(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_axes(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_buttons(Context& ctx, const std::vector<Value>& args);
    static Value gamepad_vibrationActuator(Context& ctx, const std::vector<Value>& args);
    
    // GamepadButton methods
    static Value gamepadButton_pressed(Context& ctx, const std::vector<Value>& args);
    static Value gamepadButton_touched(Context& ctx, const std::vector<Value>& args);
    static Value gamepadButton_value(Context& ctx, const std::vector<Value>& args);
    
    // GamepadHapticActuator methods (vibration)
    static Value gamepadHapticActuator_pulse(Context& ctx, const std::vector<Value>& args);
    static Value gamepadHapticActuator_playEffect(Context& ctx, const std::vector<Value>& args);
    
    // Push Notifications API - Service Worker-style push notifications with real system integration
    static Value PushManager_constructor(Context& ctx, const std::vector<Value>& args);
    static Value pushManager_subscribe(Context& ctx, const std::vector<Value>& args);
    static Value pushManager_getSubscription(Context& ctx, const std::vector<Value>& args);
    static Value pushManager_permissionState(Context& ctx, const std::vector<Value>& args);
    static Value pushManager_supportedContentEncodings(Context& ctx, const std::vector<Value>& args);
    static Value PushSubscription_constructor(Context& ctx, const std::vector<Value>& args);
    static Value pushSubscription_endpoint(Context& ctx, const std::vector<Value>& args);
    static Value pushSubscription_keys(Context& ctx, const std::vector<Value>& args);
    static Value pushSubscription_options(Context& ctx, const std::vector<Value>& args);
    static Value pushSubscription_unsubscribe(Context& ctx, const std::vector<Value>& args);
    static Value pushSubscription_toJSON(Context& ctx, const std::vector<Value>& args);
    static Value ServiceWorkerRegistration_pushManager(Context& ctx, const std::vector<Value>& args);
    static Value navigator_serviceWorker(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_register(Context& ctx, const std::vector<Value>& args);
    static Value serviceWorker_ready(Context& ctx, const std::vector<Value>& args);
    static Value PushEvent_constructor(Context& ctx, const std::vector<Value>& args);
    static Value pushEvent_data(Context& ctx, const std::vector<Value>& args);
    
    // PushMessageData methods
    static Value PushMessageData_arrayBuffer(Context& ctx, const std::vector<Value>& args);
    static Value PushMessageData_blob(Context& ctx, const std::vector<Value>& args);
    static Value PushMessageData_json(Context& ctx, const std::vector<Value>& args);
    static Value PushMessageData_text(Context& ctx, const std::vector<Value>& args);
    
    // NotificationOptions for push notifications
    static Value NotificationOptions_actions(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_badge(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_data(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_image(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_renotify(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_requireInteraction(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_tag(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_timestamp(Context& ctx, const std::vector<Value>& args);
    static Value NotificationOptions_vibrate(Context& ctx, const std::vector<Value>& args);
    
private:
    static int timer_id_counter_;
    static std::vector<std::chrono::time_point<std::chrono::steady_clock>> timer_times_;
};

} // namespace Quanta

#endif // QUANTA_WEBAPI_H