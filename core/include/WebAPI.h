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
    
private:
    static int timer_id_counter_;
    static std::vector<std::chrono::time_point<std::chrono::steady_clock>> timer_times_;
};

} // namespace Quanta

#endif // QUANTA_WEBAPI_H