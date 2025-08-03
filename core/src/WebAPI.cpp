#include "../include/WebAPI.h"
#include "Object.h"
#include "AST.h"
#include <iostream>
#include <sstream>
#include <map>
#include <iomanip>

namespace Quanta {

int WebAPI::timer_id_counter_ = 1;
std::vector<std::chrono::time_point<std::chrono::steady_clock>> WebAPI::timer_times_;

// Timer APIs
Value WebAPI::setTimeout(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.size() < 2) {
        std::cout << "setTimeout: Missing callback or delay" << std::endl;
        return Value(0.0);
    }
    
    Value callback = args[0];
    double delay = args[1].to_number();
    
    std::cout << "setTimeout: Scheduled callback to run after " << delay << "ms (simulated)" << std::endl;
    
    // In a real implementation, this would schedule the callback
    // For now, we'll just return a timer ID
    return Value(static_cast<double>(timer_id_counter_++));
}

Value WebAPI::setInterval(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.size() < 2) {
        std::cout << "setInterval: Missing callback or delay" << std::endl;
        return Value(0.0);
    }
    
    Value callback = args[0];
    double delay = args[1].to_number();
    
    std::cout << "setInterval: Scheduled callback to run every " << delay << "ms (simulated)" << std::endl;
    
    return Value(static_cast<double>(timer_id_counter_++));
}

Value WebAPI::clearTimeout(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.empty()) {
        std::cout << "clearTimeout: Missing timer ID" << std::endl;
        return Value();
    }
    
    double timer_id = args[0].to_number();
    std::cout << "clearTimeout: Cleared timer " << timer_id << " (simulated)" << std::endl;
    
    return Value();
}

Value WebAPI::clearInterval(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.empty()) {
        std::cout << "clearInterval: Missing timer ID" << std::endl;
        return Value();
    }
    
    double timer_id = args[0].to_number();
    std::cout << "clearInterval: Cleared interval " << timer_id << " (simulated)" << std::endl;
    
    return Value();
}

// Enhanced Console API
Value WebAPI::console_log(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_error(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::cout << "ERROR: ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_warn(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::cout << "WARNING: ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_info(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::cout << "INFO: ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_debug(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    // std::cout << "DEBUG: ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_trace(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::cout << "TRACE: ";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    std::cout << "    at <anonymous> (simulated stack trace)" << std::endl;
    return Value();
}

Value WebAPI::console_time(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string label = args.empty() ? "default" : args[0].to_string();
    timer_times_.push_back(std::chrono::steady_clock::now());
    std::cout << "Timer '" << label << "' started" << std::endl;
    return Value();
}

Value WebAPI::console_timeEnd(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string label = args.empty() ? "default" : args[0].to_string();
    if (!timer_times_.empty()) {
        auto end_time = std::chrono::steady_clock::now();
        auto start_time = timer_times_.back();
        timer_times_.pop_back();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "Timer '" << label << "': " << duration.count() << "ms" << std::endl;
    } else {
        std::cout << "Timer '" << label << "' does not exist" << std::endl;
    }
    return Value();
}

// Fetch API (basic simulation)
Value WebAPI::fetch(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "fetch: Missing URL" << std::endl;
        return Value("Error: Missing URL");
    }
    
    std::string url = args[0].to_string();
    std::cout << "fetch: Simulated request to " << url << std::endl;
    std::cout << "fetch: Returning simulated response" << std::endl;
    
    // Simulate a successful response
    return Value("{ \"status\": 200, \"data\": \"Simulated response from " + url + "\" }");
}

// URL API
Value WebAPI::URL_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "URL: Missing URL string" << std::endl;
        return Value("Error: Missing URL");
    }
    
    std::string url = args[0].to_string();
    std::cout << "URL: Created URL object for " << url << std::endl;
    
    return Value("URL object: " + url);
}

// Basic DOM API
Value WebAPI::document_getElementById(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "getElementById: Missing element ID" << std::endl;
        return Value();
    }
    
    std::string id = args[0].to_string();
    std::cout << "getElementById: Looking for element with ID '" << id << "' (simulated)" << std::endl;
    
    return Value("Element with ID: " + id);
}

Value WebAPI::document_createElement(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "createElement: Missing tag name" << std::endl;
        return Value();
    }
    
    std::string tagName = args[0].to_string();
    std::cout << "createElement: Created <" << tagName << "> element (simulated)" << std::endl;
    
    // Create a proper element object with properties
    auto element = ObjectFactory::create_object();
    element->set_property("tagName", Value(tagName));
    element->set_property("textContent", Value(""));
    element->set_property("onclick", Value()); // null initially
    element->set_property("id", Value(""));
    element->set_property("className", Value(""));
    element->set_property("href", Value(""));
    
    // Add click method to simulate clicking
    auto click_fn = ObjectFactory::create_native_function("click", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            std::cout << "Element clicked!" << std::endl;
            return Value();
        });
    element->set_property("click", Value(click_fn.release()));
    
    return Value(element.release());
}

Value WebAPI::document_querySelector(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "querySelector: Missing selector" << std::endl;
        return Value();
    }
    
    std::string selector = args[0].to_string();
    std::cout << "querySelector: Looking for '" << selector << "' (simulated)" << std::endl;
    
    return Value("Element matching: " + selector);
}

// Window API
Value WebAPI::window_alert(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string message = args.empty() ? "Alert!" : args[0].to_string();
    std::cout << "ALERT: " << message << std::endl;
    return Value();
}

Value WebAPI::window_confirm(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string message = args.empty() ? "Confirm?" : args[0].to_string();
    std::cout << "CONFIRM: " << message << " (returning true)" << std::endl;
    return Value(true);
}

Value WebAPI::window_prompt(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    std::string message = args.empty() ? "Enter value:" : args[0].to_string();
    std::cout << "PROMPT: " << message << " (returning 'user input')" << std::endl;
    return Value("user input");
}

// Storage API
Value WebAPI::localStorage_getItem(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "localStorage.getItem: Missing key" << std::endl;
        return Value();
    }
    
    std::string key = args[0].to_string();
    std::cout << "localStorage.getItem: Getting '" << key << "' (simulated)" << std::endl;
    
    // Simulate stored value
    return Value("stored_value_for_" + key);
}

Value WebAPI::localStorage_setItem(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "localStorage.setItem: Missing key or value" << std::endl;
        return Value();
    }
    
    std::string key = args[0].to_string();
    std::string value = args[1].to_string();
    std::cout << "localStorage.setItem: Set '" << key << "' = '" << value << "' (simulated)" << std::endl;
    
    return Value();
}

Value WebAPI::localStorage_removeItem(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "localStorage.removeItem: Missing key" << std::endl;
        return Value();
    }
    
    std::string key = args[0].to_string();
    std::cout << "localStorage.removeItem: Removed '" << key << "' (simulated)" << std::endl;
    
    return Value();
}

Value WebAPI::localStorage_clear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "localStorage.clear: Cleared all storage (simulated)" << std::endl;
    return Value();
}

// Event system
Value WebAPI::addEventListener(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "addEventListener: Missing event type or listener" << std::endl;
        return Value();
    }
    
    std::string eventType = args[0].to_string();
    std::cout << "addEventListener: Added listener for '" << eventType << "' (simulated)" << std::endl;
    
    return Value();
}

Value WebAPI::removeEventListener(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "removeEventListener: Missing event type or listener" << std::endl;
        return Value();
    }
    
    std::string eventType = args[0].to_string();
    std::cout << "removeEventListener: Removed listener for '" << eventType << "' (simulated)" << std::endl;
    
    return Value();
}

Value WebAPI::dispatchEvent(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "dispatchEvent: Missing event" << std::endl;
        return Value(false);
    }
    
    std::string event = args[0].to_string();
    std::cout << "dispatchEvent: Dispatched '" << event << "' (simulated)" << std::endl;
    
    return Value(true);
}

} // namespace Quanta