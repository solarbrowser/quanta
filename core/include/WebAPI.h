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
    
    // Fetch API (basic implementation)
    static Value fetch(Context& ctx, const std::vector<Value>& args);
    
    // URL API
    static Value URL_constructor(Context& ctx, const std::vector<Value>& args);
    
    // Basic DOM API
    static Value document_getElementById(Context& ctx, const std::vector<Value>& args);
    static Value document_createElement(Context& ctx, const std::vector<Value>& args);
    static Value document_querySelector(Context& ctx, const std::vector<Value>& args);
    static Value create_dom_element(const std::string& tagName, const std::string& id);
    
    // Window API
    static Value window_alert(Context& ctx, const std::vector<Value>& args);
    static Value window_confirm(Context& ctx, const std::vector<Value>& args);
    static Value window_prompt(Context& ctx, const std::vector<Value>& args);
    
    // Storage API
    static Value localStorage_getItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_setItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_removeItem(Context& ctx, const std::vector<Value>& args);
    static Value localStorage_clear(Context& ctx, const std::vector<Value>& args);
    
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
    
private:
    static int timer_id_counter_;
    static std::vector<std::chrono::time_point<std::chrono::steady_clock>> timer_times_;
};

} // namespace Quanta

#endif // QUANTA_WEBAPI_H