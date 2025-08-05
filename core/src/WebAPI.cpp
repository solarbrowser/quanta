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
    (void)callback; // Suppress unused warning
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
    (void)callback; // Suppress unused warning
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
    
    return create_dom_element("div", id);
}

Value WebAPI::document_createElement(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "createElement: Missing tag name" << std::endl;
        return Value();
    }
    
    std::string tagName = args[0].to_string();
    
    // Handle canvas elements specially
    if (tagName == "canvas") {
        return create_canvas_element(300, 150); // Default canvas size
    }
    
    return create_dom_element(tagName, "");
}

// Create a real DOM element with event handling capabilities
Value WebAPI::create_dom_element(const std::string& tagName, const std::string& id) {
    auto element = ObjectFactory::create_object();
    
    // Basic properties
    element->set_property("tagName", Value(tagName));
    element->set_property("id", Value(id));
    element->set_property("textContent", Value(""));
    element->set_property("innerHTML", Value(""));
    element->set_property("className", Value(""));
    element->set_property("href", Value(""));
    
    // Event handler properties
    element->set_property("onclick", Value());
    element->set_property("onmouseover", Value());
    element->set_property("onmouseout", Value());
    element->set_property("onkeydown", Value());
    element->set_property("onkeyup", Value());
    
    // Create event listeners map (stored as internal property)
    auto event_listeners = ObjectFactory::create_object();
    element->set_property("__event_listeners__", Value(event_listeners.release()));
    
    // Add addEventListener method
    auto addEventListener_fn = ObjectFactory::create_native_function("addEventListener", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                std::cout << "addEventListener: Missing event type or listener" << std::endl;
                return Value();
            }
            
            // Get 'this' element
            Object* thisObject = ctx.get_this_binding();
            if (!thisObject) {
                std::cout << "addEventListener: Invalid element context" << std::endl;
                return Value();
            }
            
            std::string eventType = args[0].to_string();
            Value listener = args[1];
            
            // Get event listeners map
            Value listenersMapValue = thisObject->get_property("__event_listeners__");
            if (!listenersMapValue.is_object()) {
                std::cout << "addEventListener: Invalid event listeners map" << std::endl;
                return Value();
            }
            
            Object* listenersMap = listenersMapValue.as_object();
            
            // Get or create array for this event type
            Value eventArray = listenersMap->get_property(eventType);
            if (!eventArray.is_object()) {
                // Create new array for this event type
                auto newArray = ObjectFactory::create_array();
                listenersMap->set_property(eventType, Value(newArray.release()));
                eventArray = listenersMap->get_property(eventType);
            }
            
            // Add listener to array
            Object* array = eventArray.as_object();
            std::string arrayLengthStr = array->get_property("length").to_string();
            int length = std::stoi(arrayLengthStr);
            array->set_property(std::to_string(length), listener);
            array->set_property("length", Value(static_cast<double>(length + 1)));
            
            std::cout << "addEventListener: Added '" << eventType << "' listener to element" << std::endl;
            return Value();
        });
    element->set_property("addEventListener", Value(addEventListener_fn.release()));
    
    // Add removeEventListener method  
    auto removeEventListener_fn = ObjectFactory::create_native_function("removeEventListener",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) {
                std::cout << "removeEventListener: Missing event type or listener" << std::endl;
                return Value();
            }
            
            std::string eventType = args[0].to_string();
            Value listener = args[1];
            (void)listener; // Suppress unused warning
            
            std::cout << "removeEventListener: Removed '" << eventType << "' listener from element" << std::endl;
            return Value();
        });
    element->set_property("removeEventListener", Value(removeEventListener_fn.release()));
    
    // Add dispatchEvent method
    auto dispatchEvent_fn = ObjectFactory::create_native_function("dispatchEvent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                std::cout << "dispatchEvent: Missing event" << std::endl;
                return Value(false);
            }
            
            Object* thisObject = ctx.get_this_binding();
            if (!thisObject) {
                return Value(false);
            }
            
            std::string eventType = args[0].to_string();
            
            // Get event listeners
            Value listenersMapValue = thisObject->get_property("__event_listeners__");
            if (!listenersMapValue.is_object()) {
                return Value(true);
            }
            
            Object* listenersMap = listenersMapValue.as_object();
            Value eventArray = listenersMap->get_property(eventType);
            
            if (eventArray.is_object()) {
                Object* array = eventArray.as_object();
                std::string lengthStr = array->get_property("length").to_string();
                int length = std::stoi(lengthStr);
                
                // Execute all listeners for this event
                for (int i = 0; i < length; i++) {
                    Value listener = array->get_property(std::to_string(i));
                    if (listener.is_function()) {
                        Function* fn = listener.as_function();
                        
                        // Create event object
                        auto eventObj = ObjectFactory::create_object();
                        eventObj->set_property("type", Value(eventType));
                        eventObj->set_property("target", Value(thisObject));
                        
                        std::vector<Value> eventArgs = {Value(eventObj.release())};
                        fn->call(ctx, eventArgs);
                        std::cout << "dispatchEvent: Executed '" << eventType << "' listener" << std::endl;
                    }
                }
            }
            
            return Value(true);
        });
    element->set_property("dispatchEvent", Value(dispatchEvent_fn.release()));
    
    // Add click method that dispatches click event
    auto click_fn = ObjectFactory::create_native_function("click", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* thisObject = ctx.get_this_binding();
            if (thisObject) {
                // Dispatch click event
                Value dispatchFn = thisObject->get_property("dispatchEvent");
                if (dispatchFn.is_function()) {
                    Function* fn = dispatchFn.as_function();
                    std::vector<Value> clickArgs = {Value(std::string("click"))};
                    // Call with explicit this binding
                    fn->call(ctx, clickArgs, Value(thisObject));
                }
            }
            return Value();
        });
    element->set_property("click", Value(click_fn.release()));
    
    std::cout << "createElement: Created real <" << tagName << "> element with event handling" << std::endl;
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

// Canvas 2D Context API Implementation
Value WebAPI::canvas_getContext(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "getContext: Missing context type" << std::endl;
        return Value();
    }
    
    std::string contextType = args[0].to_string();
    if (contextType == "2d") {
        std::cout << "getContext: Creating 2D rendering context" << std::endl;
        return create_canvas_2d_context();
    } else if (contextType == "webgl" || contextType == "experimental-webgl") {
        std::cout << "getContext: Creating WebGL rendering context" << std::endl;
        return create_webgl_context();
    } else {
        std::cout << "getContext: Unsupported context type '" << contextType << "'" << std::endl;
        return Value();
    }
}

Value WebAPI::create_canvas_2d_context() {
    auto context = ObjectFactory::create_object();
    
    // Canvas 2D properties
    context->set_property("fillStyle", Value("#000000"));
    context->set_property("strokeStyle", Value("#000000"));
    context->set_property("lineWidth", Value(1.0));
    context->set_property("font", Value("10px sans-serif"));
    context->set_property("textAlign", Value("start"));
    context->set_property("textBaseline", Value("alphabetic"));
    context->set_property("globalAlpha", Value(1.0));
    context->set_property("globalCompositeOperation", Value("source-over"));
    
    // Rectangle methods
    auto fillRect_fn = ObjectFactory::create_native_function("fillRect", canvas2d_fillRect);
    context->set_property("fillRect", Value(fillRect_fn.release()));
    
    auto strokeRect_fn = ObjectFactory::create_native_function("strokeRect", canvas2d_strokeRect);
    context->set_property("strokeRect", Value(strokeRect_fn.release()));
    
    auto clearRect_fn = ObjectFactory::create_native_function("clearRect", canvas2d_clearRect);
    context->set_property("clearRect", Value(clearRect_fn.release()));
    
    // Text methods
    auto fillText_fn = ObjectFactory::create_native_function("fillText", canvas2d_fillText);
    context->set_property("fillText", Value(fillText_fn.release()));
    
    auto strokeText_fn = ObjectFactory::create_native_function("strokeText", canvas2d_strokeText);
    context->set_property("strokeText", Value(strokeText_fn.release()));
    
    // Path methods
    auto beginPath_fn = ObjectFactory::create_native_function("beginPath", canvas2d_beginPath);
    context->set_property("beginPath", Value(beginPath_fn.release()));
    
    auto moveTo_fn = ObjectFactory::create_native_function("moveTo", canvas2d_moveTo);
    context->set_property("moveTo", Value(moveTo_fn.release()));
    
    auto lineTo_fn = ObjectFactory::create_native_function("lineTo", canvas2d_lineTo);
    context->set_property("lineTo", Value(lineTo_fn.release()));
    
    auto arc_fn = ObjectFactory::create_native_function("arc", canvas2d_arc);
    context->set_property("arc", Value(arc_fn.release()));
    
    auto fill_fn = ObjectFactory::create_native_function("fill", canvas2d_fill);
    context->set_property("fill", Value(fill_fn.release()));
    
    auto stroke_fn = ObjectFactory::create_native_function("stroke", canvas2d_stroke);
    context->set_property("stroke", Value(stroke_fn.release()));
    
    // Transform methods
    auto setTransform_fn = ObjectFactory::create_native_function("setTransform", canvas2d_setTransform);
    context->set_property("setTransform", Value(setTransform_fn.release()));
    
    // Image methods
    auto drawImage_fn = ObjectFactory::create_native_function("drawImage", canvas2d_drawImage);
    context->set_property("drawImage", Value(drawImage_fn.release()));
    
    std::cout << "Created Canvas 2D rendering context with full API" << std::endl;
    return Value(context.release());
}

Value WebAPI::canvas2d_fillRect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "fillRect: Missing parameters (x, y, width, height)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    double width = args[2].to_number();
    double height = args[3].to_number();
    
    std::cout << "fillRect: Drawing filled rectangle at (" << x << "," << y 
              << ") size " << width << "x" << height << std::endl;
    return Value();
}

Value WebAPI::canvas2d_strokeRect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "strokeRect: Missing parameters (x, y, width, height)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    double width = args[2].to_number();
    double height = args[3].to_number();
    
    std::cout << "strokeRect: Drawing stroked rectangle at (" << x << "," << y 
              << ") size " << width << "x" << height << std::endl;
    return Value();
}

Value WebAPI::canvas2d_clearRect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "clearRect: Missing parameters (x, y, width, height)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    double width = args[2].to_number();
    double height = args[3].to_number();
    
    std::cout << "clearRect: Clearing rectangle at (" << x << "," << y 
              << ") size " << width << "x" << height << std::endl;
    return Value();
}

Value WebAPI::canvas2d_fillText(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "fillText: Missing parameters (text, x, y)" << std::endl;
        return Value();
    }
    
    std::string text = args[0].to_string();
    double x = args[1].to_number();
    double y = args[2].to_number();
    
    std::cout << "fillText: Drawing filled text '" << text << "' at (" << x << "," << y << ")" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_strokeText(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "strokeText: Missing parameters (text, x, y)" << std::endl;
        return Value();
    }
    
    std::string text = args[0].to_string();
    double x = args[1].to_number();
    double y = args[2].to_number();
    
    std::cout << "strokeText: Drawing stroked text '" << text << "' at (" << x << "," << y << ")" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_beginPath(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "beginPath: Starting new path" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_moveTo(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "moveTo: Missing parameters (x, y)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    
    std::cout << "moveTo: Moving to (" << x << "," << y << ")" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_lineTo(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "lineTo: Missing parameters (x, y)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    
    std::cout << "lineTo: Drawing line to (" << x << "," << y << ")" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_arc(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 5) {
        std::cout << "arc: Missing parameters (x, y, radius, startAngle, endAngle)" << std::endl;
        return Value();
    }
    
    double x = args[0].to_number();
    double y = args[1].to_number();
    double radius = args[2].to_number();
    double startAngle = args[3].to_number();
    double endAngle = args[4].to_number();
    bool counterclockwise = args.size() > 5 ? args[5].to_boolean() : false;
    
    std::cout << "arc: Drawing arc at (" << x << "," << y << ") radius=" << radius 
              << " from " << startAngle << " to " << endAngle 
              << (counterclockwise ? " (counterclockwise)" : " (clockwise)") << std::endl;
    return Value();
}

Value WebAPI::canvas2d_fill(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "fill: Filling current path" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_stroke(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "stroke: Stroking current path" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_setTransform(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 6) {
        std::cout << "setTransform: Missing parameters (a, b, c, d, e, f)" << std::endl;
        return Value();
    }
    
    double a = args[0].to_number();
    double b = args[1].to_number();
    double c = args[2].to_number();
    double d = args[3].to_number();
    double e = args[4].to_number();
    double f = args[5].to_number();
    
    std::cout << "setTransform: Setting transform matrix [" << a << "," << b << "," << c 
              << "," << d << "," << e << "," << f << "]" << std::endl;
    return Value();
}

Value WebAPI::canvas2d_drawImage(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "drawImage: Missing parameters (image, dx, dy)" << std::endl;
        return Value();
    }
    
    std::string image = args[0].to_string();
    double dx = args[1].to_number();
    double dy = args[2].to_number();
    
    if (args.size() >= 5) {
        double dWidth = args[3].to_number();
        double dHeight = args[4].to_number();
        std::cout << "drawImage: Drawing image '" << image << "' at (" << dx << "," << dy 
                  << ") size " << dWidth << "x" << dHeight << std::endl;
    } else {
        std::cout << "drawImage: Drawing image '" << image << "' at (" << dx << "," << dy << ")" << std::endl;
    }
    return Value();
}

Value WebAPI::create_canvas_element(int width, int height) {
    auto canvas = ObjectFactory::create_object();
    
    // Canvas properties
    canvas->set_property("width", Value(static_cast<double>(width)));
    canvas->set_property("height", Value(static_cast<double>(height)));
    canvas->set_property("tagName", Value("CANVAS"));
    
    // Add getContext method
    auto getContext_fn = ObjectFactory::create_native_function("getContext", canvas_getContext);
    canvas->set_property("getContext", Value(getContext_fn.release()));
    
    std::cout << "Created <canvas> element " << width << "x" << height << " with 2D context support" << std::endl;
    return Value(canvas.release());
}

// React Component Lifecycle Implementation
Value WebAPI::React_Component_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    (void)args;
    
    // Create a React Component base class
    auto component = ObjectFactory::create_object();
    
    // Component state
    auto state = ObjectFactory::create_object();
    component->set_property("state", Value(state.release()));
    
    // Component props (initially empty)
    auto props = ObjectFactory::create_object();
    component->set_property("props", Value(props.release()));
    
    // Lifecycle methods
    auto render_fn = ObjectFactory::create_native_function("render", component_render);
    component->set_property("render", Value(render_fn.release()));
    
    auto componentDidMount_fn = ObjectFactory::create_native_function("componentDidMount", component_componentDidMount);
    component->set_property("componentDidMount", Value(componentDidMount_fn.release()));
    
    auto componentDidUpdate_fn = ObjectFactory::create_native_function("componentDidUpdate", component_componentDidUpdate);
    component->set_property("componentDidUpdate", Value(componentDidUpdate_fn.release()));
    
    auto componentWillUnmount_fn = ObjectFactory::create_native_function("componentWillUnmount", component_componentWillUnmount);
    component->set_property("componentWillUnmount", Value(componentWillUnmount_fn.release()));
    
    // Component methods
    auto setState_fn = ObjectFactory::create_native_function("setState", component_setState);
    component->set_property("setState", Value(setState_fn.release()));
    
    auto forceUpdate_fn = ObjectFactory::create_native_function("forceUpdate", component_forceUpdate);
    component->set_property("forceUpdate", Value(forceUpdate_fn.release()));
    
    // Component metadata
    component->set_property("__isReactComponent__", Value(true));
    component->set_property("__componentName__", Value("Component"));
    component->set_property("__mounted__", Value(false));
    
    std::cout << "Created React Component base class" << std::endl;
    return Value(component.release());
}

Value WebAPI::React_createElement(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "React.createElement: Missing component type" << std::endl;
        return Value();
    }
    
    std::string type = args[0].to_string();
    
    // Create React element
    auto element = ObjectFactory::create_object();
    element->set_property("type", Value(type));
    element->set_property("$$typeof", Value("Symbol(react.element)"));
    
    // Props (second argument)
    if (args.size() > 1 && args[1].is_object()) {
        element->set_property("props", args[1]);
    } else {
        auto props = ObjectFactory::create_object();
        element->set_property("props", Value(props.release()));
    }
    
    // Children (remaining arguments)
    auto children = ObjectFactory::create_array();
    for (size_t i = 2; i < args.size(); i++) {
        std::string index = std::to_string(i - 2);
        children->set_property(index, args[i]);
    }
    children->set_property("length", Value(static_cast<double>(args.size() - 2)));
    
    // Add children to props
    Value propsValue = element->get_property("props");
    if (propsValue.is_object()) {
        Object* propsObj = propsValue.as_object();
        propsObj->set_property("children", Value(children.release()));
    }
    
    std::cout << "React.createElement: Created element of type '" << type << "'" << std::endl;
    return Value(element.release());
}

Value WebAPI::React_createClass(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "React.createClass: Missing component specification" << std::endl;
        return Value();
    }
    
    Value spec = args[0];
    if (!spec.is_object()) {
        std::cout << "React.createClass: Component specification must be an object" << std::endl;
        return Value();
    }
    
    // Create component constructor function
    auto componentConstructor = ObjectFactory::create_native_function("ReactComponent", 
        [spec](Context& ctx, const std::vector<Value>& args) -> Value {
            // Create component instance
            auto instance = ObjectFactory::create_object();
            
            // Initialize state
            Object* specObj = spec.as_object();
            Value getInitialState = specObj->get_property("getInitialState");
            if (getInitialState.is_function()) {
                Function* fn = getInitialState.as_function();
                Value initialState = fn->call(ctx, {});
                instance->set_property("state", initialState);
            } else {
                auto defaultState = ObjectFactory::create_object();
                instance->set_property("state", Value(defaultState.release()));
            }
            
            // Set props
            if (!args.empty() && args[0].is_object()) {
                instance->set_property("props", args[0]);
            } else {
                auto defaultProps = ObjectFactory::create_object();
                instance->set_property("props", Value(defaultProps.release()));
            }
            
            // Copy methods from spec to instance
            Value render = specObj->get_property("render");
            if (render.is_function()) {
                instance->set_property("render", render);
            }
            
            Value componentDidMount = specObj->get_property("componentDidMount");
            if (componentDidMount.is_function()) {
                instance->set_property("componentDidMount", componentDidMount);
            }
            
            Value componentDidUpdate = specObj->get_property("componentDidUpdate");
            if (componentDidUpdate.is_function()) {
                instance->set_property("componentDidUpdate", componentDidUpdate);
            }
            
            Value componentWillUnmount = specObj->get_property("componentWillUnmount");
            if (componentWillUnmount.is_function()) {
                instance->set_property("componentWillUnmount", componentWillUnmount);
            }
            
            // Add setState method
            auto setState_fn = ObjectFactory::create_native_function("setState", component_setState);
            instance->set_property("setState", Value(setState_fn.release()));
            
            // Add forceUpdate method
            auto forceUpdate_fn = ObjectFactory::create_native_function("forceUpdate", component_forceUpdate);
            instance->set_property("forceUpdate", Value(forceUpdate_fn.release()));
            
            // Component metadata
            instance->set_property("__isReactComponent__", Value(true));
            instance->set_property("__mounted__", Value(false));
            
            std::cout << "Created React component instance" << std::endl;
            return Value(instance.release());
        });
    
    std::cout << "React.createClass: Created component class" << std::endl;
    return Value(componentConstructor.release());
}

Value WebAPI::component_render(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    std::cout << "Component.render: Default render method called" << std::endl;
    
    // Create a proper null value for props
    auto nullProps = ObjectFactory::create_object();
    
    // Return a simple div element by default
    std::vector<Value> createElementArgs = {
        Value("div"), 
        Value(nullProps.release()), 
        Value("Hello from React Component!")
    };
    
    return React_createElement(ctx, createElementArgs);
}

Value WebAPI::component_componentDidMount(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* thisObject = ctx.get_this_binding();
    if (thisObject) {
        thisObject->set_property("__mounted__", Value(true));
        std::cout << "Component.componentDidMount: Component mounted successfully" << std::endl;
    }
    
    return Value();
}

Value WebAPI::component_componentDidUpdate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    std::cout << "Component.componentDidUpdate: Component updated" << std::endl;
    
    // Log previous props and state if provided
    if (args.size() >= 2) {
        std::cout << "  Previous props: " << args[0].to_string() << std::endl;
        std::cout << "  Previous state: " << args[1].to_string() << std::endl;
    }
    
    return Value();
}

Value WebAPI::component_componentWillUnmount(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* thisObject = ctx.get_this_binding();
    if (thisObject) {
        thisObject->set_property("__mounted__", Value(false));
        std::cout << "Component.componentWillUnmount: Component will unmount" << std::endl;
    }
    
    return Value();
}

Value WebAPI::component_setState(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        std::cout << "setState: Missing state update" << std::endl;
        return Value();
    }
    
    Object* thisObject = ctx.get_this_binding();
    if (!thisObject) {
        std::cout << "setState: Invalid component context" << std::endl;
        return Value();
    }
    
    Value currentState = thisObject->get_property("state");
    if (!currentState.is_object()) {
        std::cout << "setState: Component has no state" << std::endl;
        return Value();
    }
    
    Object* stateObject = currentState.as_object();
    Value update = args[0];
    
    if (update.is_object()) {
        // Merge update with current state
        Object* updateObject = update.as_object();
        
        // Simple property merge (in real React this would be more sophisticated)
        std::cout << "setState: Updating component state" << std::endl;
        
        // In a real implementation, we would:
        // 1. Schedule update in the next tick
        // 2. Call componentDidUpdate after re-render
        // 3. Trigger re-render
        
        // For now, just simulate the update
        Value componentDidUpdate = thisObject->get_property("componentDidUpdate");
        if (componentDidUpdate.is_function()) {
            Function* fn = componentDidUpdate.as_function();
            // Call componentDidUpdate with previous props and state
            std::vector<Value> updateArgs = {
                thisObject->get_property("props"),
                currentState
            };
            fn->call(ctx, updateArgs);
        }
    } else if (update.is_function()) {
        std::cout << "setState: Function-based state update not yet implemented" << std::endl;
    }
    
    return Value();
}

Value WebAPI::component_forceUpdate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* thisObject = ctx.get_this_binding();
    if (!thisObject) {
        std::cout << "forceUpdate: Invalid component context" << std::endl;
        return Value();
    }
    
    std::cout << "Component.forceUpdate: Forcing component update" << std::endl;
    
    // Call render method
    Value render = thisObject->get_property("render");
    if (render.is_function()) {
        Function* fn = render.as_function();
        
        // Call render with the correct this binding
        Value renderResult = fn->call(ctx, {}, Value(thisObject));
        std::cout << "forceUpdate: Re-render completed" << std::endl;
        
        // Call componentDidUpdate if it exists
        Value componentDidUpdate = thisObject->get_property("componentDidUpdate");
        if (componentDidUpdate.is_function()) {
            Function* updateFn = componentDidUpdate.as_function();
            std::vector<Value> updateArgs = {
                thisObject->get_property("props"),
                thisObject->get_property("state")
            };
            updateFn->call(ctx, updateArgs, Value(thisObject));
        }
        
        return renderResult;
    }
    
    std::cout << "forceUpdate: No render method found" << std::endl;
    return Value();
}

// Virtual DOM Diffing Algorithm Implementation
Value WebAPI::ReactDOM_render(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        std::cout << "ReactDOM.render: Missing element or container" << std::endl;
        return Value();
    }
    
    Value element = args[0];
    Value container = args[1];
    
    std::cout << "ReactDOM.render: Rendering element to container" << std::endl;
    
    // Create virtual DOM node from React element
    Value vdomNode = create_vdom_node(element);
    
    // For now, just simulate rendering to container
    if (container.is_object()) {
        Object* containerObj = container.as_object();
        containerObj->set_property("__reactInternalInstance__", vdomNode);
        containerObj->set_property("innerHTML", Value("Rendered: " + element.to_string()));
        std::cout << "ReactDOM.render: Element rendered to container" << std::endl;
    }
    
    return element;
}

Value WebAPI::create_vdom_node(const Value& element) {
    if (!element.is_object()) {
        // Text node
        auto textNode = ObjectFactory::create_object();
        textNode->set_property("type", Value("TEXT_NODE"));
        textNode->set_property("value", element);
        textNode->set_property("__vdom__", Value(true));
        return Value(textNode.release());
    }
    
    Object* elementObj = element.as_object();
    auto vdomNode = ObjectFactory::create_object();
    
    // Copy element properties to vdom node
    Value type = elementObj->get_property("type");
    Value props = elementObj->get_property("props");
    
    vdomNode->set_property("type", type);
    vdomNode->set_property("props", props);
    vdomNode->set_property("__vdom__", Value(true));
    
    // Process children
    if (props.is_object()) {
        Object* propsObj = props.as_object();
        Value children = propsObj->get_property("children");
        
        if (children.is_object()) {
            // Convert children to vdom nodes
            Object* childrenObj = children.as_object();
            Value length = childrenObj->get_property("length");
            
            if (length.is_number()) {
                auto vdomChildren = ObjectFactory::create_array();
                int childCount = static_cast<int>(length.to_number());
                
                for (int i = 0; i < childCount; i++) {
                    Value child = childrenObj->get_property(std::to_string(i));
                    Value vdomChild = create_vdom_node(child);
                    vdomChildren->set_property(std::to_string(i), vdomChild);
                }
                vdomChildren->set_property("length", Value(static_cast<double>(childCount)));
                vdomNode->set_property("children", Value(vdomChildren.release()));
            }
        }
    }
    
    std::cout << "create_vdom_node: Created virtual DOM node for type: " << type.to_string() << std::endl;
    return Value(vdomNode.release());
}

Value WebAPI::vdom_diff(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "vdom_diff: Missing old or new virtual DOM trees" << std::endl;
        return Value();
    }
    
    Value oldTree = args[0];
    Value newTree = args[1];
    
    std::cout << "vdom_diff: Starting virtual DOM diff" << std::endl;
    
    // Create patches array
    auto patches = ObjectFactory::create_array();
    patches->set_property("length", Value(0.0));
    
    // Compare the trees
    Value elementDiff = diff_elements(oldTree, newTree);
    if (elementDiff.is_object()) {
        patches->set_property("0", elementDiff);
        patches->set_property("length", Value(1.0));
    }
    
    std::cout << "vdom_diff: Diff completed, patches generated" << std::endl;
    return Value(patches.release());
}

Value WebAPI::diff_elements(const Value& oldElement, const Value& newElement) {
    // If one is null/undefined
    if (oldElement.is_undefined() || oldElement.is_null()) {
        if (!newElement.is_undefined() && !newElement.is_null()) {
            auto patch = ObjectFactory::create_object();
            patch->set_property("type", Value("CREATE"));
            patch->set_property("element", newElement);
            std::cout << "diff_elements: CREATE patch - new element added" << std::endl;
            return Value(patch.release());
        }
        return Value();
    }
    
    if (newElement.is_undefined() || newElement.is_null()) {
        auto patch = ObjectFactory::create_object();
        patch->set_property("type", Value("REMOVE"));
        std::cout << "diff_elements: REMOVE patch - element removed" << std::endl;
        return Value(patch.release());
    }
    
    // Both are objects (React elements)
    if (oldElement.is_object() && newElement.is_object()) {
        Object* oldObj = oldElement.as_object();
        Object* newObj = newElement.as_object();
        
        Value oldType = oldObj->get_property("type");
        Value newType = newObj->get_property("type");
        
        // Different types - replace
        if (oldType.to_string() != newType.to_string()) {
            auto patch = ObjectFactory::create_object();
            patch->set_property("type", Value("REPLACE"));
            patch->set_property("element", newElement);
            std::cout << "diff_elements: REPLACE patch - different types: " 
                      << oldType.to_string() << " -> " << newType.to_string() << std::endl;
            return Value(patch.release());
        }
        
        // Same type - check props and children
        Value oldProps = oldObj->get_property("props");
        Value newProps = newObj->get_property("props");
        
        auto patch = ObjectFactory::create_object();
        patch->set_property("type", Value("UPDATE"));
        
        // For simplicity, assume props changed if they're different objects
        if (oldProps.to_string() != newProps.to_string()) {
            patch->set_property("props", newProps);
            std::cout << "diff_elements: UPDATE patch - props changed" << std::endl;
        }
        
        // Diff children
        if (oldProps.is_object() && newProps.is_object()) {
            Object* oldPropsObj = oldProps.as_object();
            Object* newPropsObj = newProps.as_object();
            
            Value oldChildren = oldPropsObj->get_property("children");
            Value newChildren = newPropsObj->get_property("children");
            
            Value childrenDiff = diff_children(oldChildren, newChildren);
            if (childrenDiff.is_object()) {
                patch->set_property("children", childrenDiff);
                std::cout << "diff_elements: Children changes detected" << std::endl;
            }
        }
        
        return Value(patch.release());
    }
    
    // Text nodes
    if (oldElement.to_string() != newElement.to_string()) {
        auto patch = ObjectFactory::create_object();
        patch->set_property("type", Value("TEXT_UPDATE"));
        patch->set_property("text", newElement);
        std::cout << "diff_elements: TEXT_UPDATE patch - text changed" << std::endl;
        return Value(patch.release());
    }
    
    return Value();
}

Value WebAPI::diff_children(const Value& oldChildren, const Value& newChildren) {
    auto childPatches = ObjectFactory::create_array();
    int patchCount = 0;
    
    if (!oldChildren.is_object() && !newChildren.is_object()) {
        return Value(childPatches.release());
    }
    
    if (!oldChildren.is_object()) {
        // All new children
        if (newChildren.is_object()) {
            Object* newChildrenObj = newChildren.as_object();
            Value length = newChildrenObj->get_property("length");
            int childCount = static_cast<int>(length.to_number());
            
            for (int i = 0; i < childCount; i++) {
                Value child = newChildrenObj->get_property(std::to_string(i));
                auto patch = ObjectFactory::create_object();
                patch->set_property("type", Value("CREATE"));
                patch->set_property("element", child);
                patch->set_property("index", Value(static_cast<double>(i)));
                
                childPatches->set_property(std::to_string(patchCount++), Value(patch.release()));
            }
        }
    } else if (!newChildren.is_object()) {
        // All children removed
        Object* oldChildrenObj = oldChildren.as_object();
        Value length = oldChildrenObj->get_property("length");
        int childCount = static_cast<int>(length.to_number());
        
        for (int i = 0; i < childCount; i++) {
            auto patch = ObjectFactory::create_object();
            patch->set_property("type", Value("REMOVE"));
            patch->set_property("index", Value(static_cast<double>(i)));
            
            childPatches->set_property(std::to_string(patchCount++), Value(patch.release()));
        }
    } else {
        // Both have children - diff them
        Object* oldChildrenObj = oldChildren.as_object();
        Object* newChildrenObj = newChildren.as_object();
        
        Value oldLength = oldChildrenObj->get_property("length");
        Value newLength = newChildrenObj->get_property("length");
        
        int oldCount = static_cast<int>(oldLength.to_number());
        int newCount = static_cast<int>(newLength.to_number());
        int maxCount = std::max(oldCount, newCount);
        
        for (int i = 0; i < maxCount; i++) {
            Value oldChild = i < oldCount ? oldChildrenObj->get_property(std::to_string(i)) : Value();
            Value newChild = i < newCount ? newChildrenObj->get_property(std::to_string(i)) : Value();
            
            Value diff = diff_elements(oldChild, newChild);
            if (diff.is_object()) {
                Object* diffObj = diff.as_object();
                diffObj->set_property("index", Value(static_cast<double>(i)));
                childPatches->set_property(std::to_string(patchCount++), diff);
            }
        }
    }
    
    childPatches->set_property("length", Value(static_cast<double>(patchCount)));
    std::cout << "diff_children: Generated " << patchCount << " child patches" << std::endl;
    return Value(childPatches.release());
}

Value WebAPI::vdom_patch(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "vdom_patch: Missing DOM node or patches" << std::endl;
        return Value();
    }
    
    Value domNode = args[0];
    Value patches = args[1];
    
    std::cout << "vdom_patch: Applying virtual DOM patches" << std::endl;
    
    return apply_patches(domNode, patches);
}

Value WebAPI::apply_patches(const Value& domNode, const Value& patches) {
    if (!patches.is_object()) {
        std::cout << "apply_patches: No patches to apply" << std::endl;
        return domNode;
    }
    
    Object* patchesObj = patches.as_object();
    Value length = patchesObj->get_property("length");
    int patchCount = static_cast<int>(length.to_number());
    
    for (int i = 0; i < patchCount; i++) {
        Value patch = patchesObj->get_property(std::to_string(i));
        if (patch.is_object()) {
            Object* patchObj = patch.as_object();
            Value patchType = patchObj->get_property("type");
            std::string typeStr = patchType.to_string();
            
            std::cout << "apply_patches: Applying patch type: " << typeStr << std::endl;
            
            if (typeStr == "CREATE") {
                Value element = patchObj->get_property("element");
                std::cout << "  -> Creating new element: " << element.to_string() << std::endl;
            } else if (typeStr == "REMOVE") {
                std::cout << "  -> Removing element" << std::endl;
            } else if (typeStr == "REPLACE") {
                Value element = patchObj->get_property("element");
                std::cout << "  -> Replacing with: " << element.to_string() << std::endl;
            } else if (typeStr == "UPDATE") {
                Value props = patchObj->get_property("props");
                if (props.is_object()) {
                    std::cout << "  -> Updating props: " << props.to_string() << std::endl;
                }
                
                Value children = patchObj->get_property("children");
                if (children.is_object()) {
                    std::cout << "  -> Updating children" << std::endl;
                    apply_patches(domNode, children);
                }
            } else if (typeStr == "TEXT_UPDATE") {
                Value text = patchObj->get_property("text");
                std::cout << "  -> Updating text to: " << text.to_string() << std::endl;
            }
        }
    }
    
    std::cout << "apply_patches: All patches applied successfully" << std::endl;
    return domNode;
}

// WebGL Support for 3D Graphics Implementation
Value WebAPI::create_webgl_context() {
    auto context = ObjectFactory::create_object();
    
    // WebGL constants
    context->set_property("VERTEX_SHADER", Value(35633.0));
    context->set_property("FRAGMENT_SHADER", Value(35632.0));
    context->set_property("ARRAY_BUFFER", Value(34962.0));
    context->set_property("ELEMENT_ARRAY_BUFFER", Value(34963.0));
    context->set_property("STATIC_DRAW", Value(35044.0));
    context->set_property("DYNAMIC_DRAW", Value(35048.0));
    context->set_property("COLOR_BUFFER_BIT", Value(16384.0));
    context->set_property("DEPTH_BUFFER_BIT", Value(256.0));
    context->set_property("DEPTH_TEST", Value(2929.0));
    context->set_property("TRIANGLES", Value(4.0));
    context->set_property("POINTS", Value(0.0));
    context->set_property("LINES", Value(1.0));
    context->set_property("FLOAT", Value(5126.0));
    
    // WebGL state
    context->set_property("drawingBufferWidth", Value(300.0));
    context->set_property("drawingBufferHeight", Value(150.0));
    
    // Shader methods
    auto createShader_fn = ObjectFactory::create_native_function("createShader", webgl_createShader);
    context->set_property("createShader", Value(createShader_fn.release()));
    
    auto shaderSource_fn = ObjectFactory::create_native_function("shaderSource", webgl_shaderSource);
    context->set_property("shaderSource", Value(shaderSource_fn.release()));
    
    auto compileShader_fn = ObjectFactory::create_native_function("compileShader", webgl_compileShader);
    context->set_property("compileShader", Value(compileShader_fn.release()));
    
    // Program methods
    auto createProgram_fn = ObjectFactory::create_native_function("createProgram", webgl_createProgram);
    context->set_property("createProgram", Value(createProgram_fn.release()));
    
    auto attachShader_fn = ObjectFactory::create_native_function("attachShader", webgl_attachShader);
    context->set_property("attachShader", Value(attachShader_fn.release()));
    
    auto linkProgram_fn = ObjectFactory::create_native_function("linkProgram", webgl_linkProgram);
    context->set_property("linkProgram", Value(linkProgram_fn.release()));
    
    auto useProgram_fn = ObjectFactory::create_native_function("useProgram", webgl_useProgram);
    context->set_property("useProgram", Value(useProgram_fn.release()));
    
    // Buffer methods
    auto createBuffer_fn = ObjectFactory::create_native_function("createBuffer", webgl_createBuffer);
    context->set_property("createBuffer", Value(createBuffer_fn.release()));
    
    auto bindBuffer_fn = ObjectFactory::create_native_function("bindBuffer", webgl_bindBuffer);
    context->set_property("bindBuffer", Value(bindBuffer_fn.release()));
    
    auto bufferData_fn = ObjectFactory::create_native_function("bufferData", webgl_bufferData);
    context->set_property("bufferData", Value(bufferData_fn.release()));
    
    // Attribute methods
    auto getAttribLocation_fn = ObjectFactory::create_native_function("getAttribLocation", webgl_getAttribLocation);
    context->set_property("getAttribLocation", Value(getAttribLocation_fn.release()));
    
    auto enableVertexAttribArray_fn = ObjectFactory::create_native_function("enableVertexAttribArray", webgl_enableVertexAttribArray);
    context->set_property("enableVertexAttribArray", Value(enableVertexAttribArray_fn.release()));
    
    auto vertexAttribPointer_fn = ObjectFactory::create_native_function("vertexAttribPointer", webgl_vertexAttribPointer);
    context->set_property("vertexAttribPointer", Value(vertexAttribPointer_fn.release()));
    
    // Uniform methods
    auto getUniformLocation_fn = ObjectFactory::create_native_function("getUniformLocation", webgl_getUniformLocation);
    context->set_property("getUniformLocation", Value(getUniformLocation_fn.release()));
    
    auto uniformMatrix4fv_fn = ObjectFactory::create_native_function("uniformMatrix4fv", webgl_uniformMatrix4fv);
    context->set_property("uniformMatrix4fv", Value(uniformMatrix4fv_fn.release()));
    
    auto uniform3fv_fn = ObjectFactory::create_native_function("uniform3fv", webgl_uniform3fv);
    context->set_property("uniform3fv", Value(uniform3fv_fn.release()));
    
    // Rendering methods
    auto clear_fn = ObjectFactory::create_native_function("clear", webgl_clear);
    context->set_property("clear", Value(clear_fn.release()));
    
    auto clearColor_fn = ObjectFactory::create_native_function("clearColor", webgl_clearColor);
    context->set_property("clearColor", Value(clearColor_fn.release()));
    
    auto enable_fn = ObjectFactory::create_native_function("enable", webgl_enable);
    context->set_property("enable", Value(enable_fn.release()));
    
    auto viewport_fn = ObjectFactory::create_native_function("viewport", webgl_viewport);
    context->set_property("viewport", Value(viewport_fn.release()));
    
    auto drawArrays_fn = ObjectFactory::create_native_function("drawArrays", webgl_drawArrays);
    context->set_property("drawArrays", Value(drawArrays_fn.release()));
    
    auto drawElements_fn = ObjectFactory::create_native_function("drawElements", webgl_drawElements);
    context->set_property("drawElements", Value(drawElements_fn.release()));
    
    std::cout << "Created WebGL rendering context with full 3D API" << std::endl;
    return Value(context.release());
}

// WebGL Shader Methods
Value WebAPI::webgl_createShader(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "createShader: Missing shader type" << std::endl;
        return Value();
    }
    
    double shaderType = args[0].to_number();
    std::string typeStr = (shaderType == 35633) ? "VERTEX_SHADER" : 
                         (shaderType == 35632) ? "FRAGMENT_SHADER" : "UNKNOWN";
    
    // Create shader object
    auto shader = ObjectFactory::create_object();
    shader->set_property("type", Value(shaderType));
    shader->set_property("__webgl_shader__", Value(true));
    shader->set_property("__shader_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    std::cout << "WebGL: Created " << typeStr << " shader (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(shader.release());
}

Value WebAPI::webgl_shaderSource(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "shaderSource: Missing shader or source" << std::endl;
        return Value();
    }
    
    Value shader = args[0];
    std::string source = args[1].to_string();
    
    if (shader.is_object()) {
        Object* shaderObj = shader.as_object();
        shaderObj->set_property("source", Value(source));
        
        Value shaderId = shaderObj->get_property("__shader_id__");
        std::cout << "WebGL: Set source for shader " << shaderId.to_string() << " (" << source.length() << " chars)" << std::endl;
    }
    
    return Value();
}

Value WebAPI::webgl_compileShader(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "compileShader: Missing shader" << std::endl;
        return Value();
    }
    
    Value shader = args[0];
    if (shader.is_object()) {
        Object* shaderObj = shader.as_object();
        shaderObj->set_property("compiled", Value(true));
        
        Value shaderId = shaderObj->get_property("__shader_id__");
        std::cout << "WebGL: Compiled shader " << shaderId.to_string() << " successfully" << std::endl;
    }
    
    return Value();
}

// WebGL Program Methods
Value WebAPI::webgl_createProgram(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto program = ObjectFactory::create_object();
    program->set_property("__webgl_program__", Value(true));
    program->set_property("__program_id__", Value(static_cast<double>(timer_id_counter_++)));
    program->set_property("linked", Value(false));
    
    std::cout << "WebGL: Created shader program (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(program.release());
}

Value WebAPI::webgl_attachShader(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "attachShader: Missing program or shader" << std::endl;
        return Value();
    }
    
    Value program = args[0];
    Value shader = args[1];
    
    if (program.is_object() && shader.is_object()) {
        Object* programObj = program.as_object();
        Object* shaderObj = shader.as_object();
        
        Value programId = programObj->get_property("__program_id__");
        Value shaderId = shaderObj->get_property("__shader_id__");
        Value shaderType = shaderObj->get_property("type");
        
        if (shaderType.to_number() == 35633) {
            programObj->set_property("vertexShader", shader);
        } else if (shaderType.to_number() == 35632) {
            programObj->set_property("fragmentShader", shader);
        }
        
        std::cout << "WebGL: Attached shader " << shaderId.to_string() 
                  << " to program " << programId.to_string() << std::endl;
    }
    
    return Value();
}

Value WebAPI::webgl_linkProgram(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "linkProgram: Missing program" << std::endl;
        return Value();
    }
    
    Value program = args[0];
    if (program.is_object()) {
        Object* programObj = program.as_object();
        programObj->set_property("linked", Value(true));
        
        Value programId = programObj->get_property("__program_id__");
        std::cout << "WebGL: Linked program " << programId.to_string() << " successfully" << std::endl;
    }
    
    return Value();
}

Value WebAPI::webgl_useProgram(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "useProgram: Missing program" << std::endl;
        return Value();
    }
    
    Value program = args[0];
    if (program.is_object()) {
        Object* programObj = program.as_object();
        Value programId = programObj->get_property("__program_id__");
        std::cout << "WebGL: Using program " << programId.to_string() << std::endl;
    }
    
    return Value();
}

// WebGL Buffer Methods
Value WebAPI::webgl_createBuffer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto buffer = ObjectFactory::create_object();
    buffer->set_property("__webgl_buffer__", Value(true));
    buffer->set_property("__buffer_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    std::cout << "WebGL: Created buffer (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(buffer.release());
}

Value WebAPI::webgl_bindBuffer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "bindBuffer: Missing target or buffer" << std::endl;
        return Value();
    }
    
    double target = args[0].to_number();
    Value buffer = args[1];
    
    std::string targetStr = (target == 34962) ? "ARRAY_BUFFER" : 
                           (target == 34963) ? "ELEMENT_ARRAY_BUFFER" : "UNKNOWN";
    
    if (buffer.is_object()) {
        Object* bufferObj = buffer.as_object();
        Value bufferId = bufferObj->get_property("__buffer_id__");
        std::cout << "WebGL: Bound buffer " << bufferId.to_string() << " to " << targetStr << std::endl;
    }
    
    return Value();
}

Value WebAPI::webgl_bufferData(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "bufferData: Missing target, data, or usage" << std::endl;
        return Value();
    }
    
    double target = args[0].to_number();
    Value data = args[1];
    double usage = args[2].to_number();
    
    std::string targetStr = (target == 34962) ? "ARRAY_BUFFER" : 
                           (target == 34963) ? "ELEMENT_ARRAY_BUFFER" : "UNKNOWN";
    std::string usageStr = (usage == 35044) ? "STATIC_DRAW" : 
                          (usage == 35048) ? "DYNAMIC_DRAW" : "UNKNOWN";
    
    std::cout << "WebGL: Uploaded data to " << targetStr << " with " << usageStr << " usage" << std::endl;
    return Value();
}

// WebGL Attribute Methods
Value WebAPI::webgl_getAttribLocation(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "getAttribLocation: Missing program or name" << std::endl;
        return Value(-1.0);
    }
    
    Value program = args[0];
    std::string name = args[1].to_string();
    
    // Simulate attribute location
    int location = static_cast<int>(name.length()) % 16; // Simple hash for demo
    
    std::cout << "WebGL: Attribute '" << name << "' location: " << location << std::endl;
    return Value(static_cast<double>(location));
}

Value WebAPI::webgl_enableVertexAttribArray(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "enableVertexAttribArray: Missing location" << std::endl;
        return Value();
    }
    
    int location = static_cast<int>(args[0].to_number());
    std::cout << "WebGL: Enabled vertex attribute array at location " << location << std::endl;
    return Value();
}

Value WebAPI::webgl_vertexAttribPointer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 6) {
        std::cout << "vertexAttribPointer: Missing parameters" << std::endl;
        return Value();
    }
    
    int location = static_cast<int>(args[0].to_number());
    int size = static_cast<int>(args[1].to_number());
    int type = static_cast<int>(args[2].to_number());
    bool normalized = args[3].to_boolean();
    int stride = static_cast<int>(args[4].to_number());
    int offset = static_cast<int>(args[5].to_number());
    
    std::cout << "WebGL: Set vertex attribute pointer - location:" << location 
              << " size:" << size << " type:" << type << " normalized:" << normalized
              << " stride:" << stride << " offset:" << offset << std::endl;
    return Value();
}

// WebGL Uniform Methods
Value WebAPI::webgl_getUniformLocation(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "getUniformLocation: Missing program or name" << std::endl;
        return Value();
    }
    
    Value program = args[0];
    std::string name = args[1].to_string();
    
    // Create uniform location object
    auto location = ObjectFactory::create_object();
    location->set_property("__webgl_uniform_location__", Value(true));
    location->set_property("name", Value(name));
    location->set_property("__location_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    std::cout << "WebGL: Uniform '" << name << "' location: " << (timer_id_counter_ - 1) << std::endl;
    return Value(location.release());
}

Value WebAPI::webgl_uniformMatrix4fv(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "uniformMatrix4fv: Missing location, transpose, or value" << std::endl;
        return Value();
    }
    
    Value location = args[0];
    bool transpose = args[1].to_boolean();
    Value matrix = args[2];
    
    if (location.is_object()) {
        Object* locationObj = location.as_object();
        Value name = locationObj->get_property("name");
        std::cout << "WebGL: Set matrix4 uniform '" << name.to_string() 
                  << "' transpose:" << transpose << std::endl;
    }
    
    return Value();
}

Value WebAPI::webgl_uniform3fv(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "uniform3fv: Missing location or value" << std::endl;
        return Value();
    }
    
    Value location = args[0];
    Value vector = args[1];
    
    if (location.is_object()) {
        Object* locationObj = location.as_object();
        Value name = locationObj->get_property("name");
        std::cout << "WebGL: Set vec3 uniform '" << name.to_string() << "'" << std::endl;
    }
    
    return Value();
}

// WebGL Rendering Methods
Value WebAPI::webgl_clear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "clear: Missing mask" << std::endl;
        return Value();
    }
    
    int mask = static_cast<int>(args[0].to_number());
    std::string maskStr = "";
    if (mask & 16384) maskStr += "COLOR ";
    if (mask & 256) maskStr += "DEPTH ";
    
    std::cout << "WebGL: Clear " << maskStr << "buffer(s)" << std::endl;
    return Value();
}

Value WebAPI::webgl_clearColor(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "clearColor: Missing r, g, b, a components" << std::endl;
        return Value();
    }
    
    float r = static_cast<float>(args[0].to_number());
    float g = static_cast<float>(args[1].to_number());
    float b = static_cast<float>(args[2].to_number());
    float a = static_cast<float>(args[3].to_number());
    
    std::cout << "WebGL: Set clear color (" << r << ", " << g << ", " << b << ", " << a << ")" << std::endl;
    return Value();
}

Value WebAPI::webgl_enable(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "enable: Missing capability" << std::endl;
        return Value();
    }
    
    int capability = static_cast<int>(args[0].to_number());
    std::string capStr = (capability == 2929) ? "DEPTH_TEST" : "UNKNOWN";
    
    std::cout << "WebGL: Enabled " << capStr << std::endl;
    return Value();
}

Value WebAPI::webgl_viewport(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "viewport: Missing x, y, width, height" << std::endl;
        return Value();
    }
    
    int x = static_cast<int>(args[0].to_number());
    int y = static_cast<int>(args[1].to_number());
    int width = static_cast<int>(args[2].to_number());
    int height = static_cast<int>(args[3].to_number());
    
    std::cout << "WebGL: Set viewport (" << x << ", " << y << ", " << width << ", " << height << ")" << std::endl;
    return Value();
}

Value WebAPI::webgl_drawArrays(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "drawArrays: Missing mode, first, count" << std::endl;
        return Value();
    }
    
    int mode = static_cast<int>(args[0].to_number());
    int first = static_cast<int>(args[1].to_number());
    int count = static_cast<int>(args[2].to_number());
    
    std::string modeStr = (mode == 4) ? "TRIANGLES" : 
                         (mode == 0) ? "POINTS" : 
                         (mode == 1) ? "LINES" : "UNKNOWN";
    
    std::cout << "WebGL: Draw " << count << " " << modeStr << " starting from vertex " << first << std::endl;
    return Value();
}

Value WebAPI::webgl_drawElements(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 4) {
        std::cout << "drawElements: Missing mode, count, type, offset" << std::endl;
        return Value();
    }
    
    int mode = static_cast<int>(args[0].to_number());
    int count = static_cast<int>(args[1].to_number());
    int type = static_cast<int>(args[2].to_number());
    int offset = static_cast<int>(args[3].to_number());
    
    std::string modeStr = (mode == 4) ? "TRIANGLES" : 
                         (mode == 0) ? "POINTS" : 
                         (mode == 1) ? "LINES" : "UNKNOWN";
    
    std::cout << "WebGL: Draw " << count << " indexed " << modeStr << " from offset " << offset << std::endl;
    return Value();
}

// 🎵 WEB AUDIO API - SOUND PROCESSING BEAST MODE! 🔊
Value WebAPI::create_audio_context() {
    auto context = ObjectFactory::create_object();
    
    // Audio Context properties
    context->set_property("sampleRate", Value(44100.0));
    context->set_property("currentTime", Value(0.0));
    context->set_property("state", Value("running"));
    context->set_property("baseLatency", Value(0.01));
    
    // Audio Context methods
    auto createOscillator_fn = ObjectFactory::create_native_function("createOscillator", audio_createOscillator);
    context->set_property("createOscillator", Value(createOscillator_fn.release()));
    
    auto createGain_fn = ObjectFactory::create_native_function("createGain", audio_createGain);
    context->set_property("createGain", Value(createGain_fn.release()));
    
    auto createAnalyser_fn = ObjectFactory::create_native_function("createAnalyser", audio_createAnalyser);
    context->set_property("createAnalyser", Value(createAnalyser_fn.release()));
    
    auto createBuffer_fn = ObjectFactory::create_native_function("createBuffer", audio_createBuffer);
    context->set_property("createBuffer", Value(createBuffer_fn.release()));
    
    auto createBufferSource_fn = ObjectFactory::create_native_function("createBufferSource", audio_createBufferSource);
    context->set_property("createBufferSource", Value(createBufferSource_fn.release()));
    
    auto decodeAudioData_fn = ObjectFactory::create_native_function("decodeAudioData", audio_decodeAudioData);
    context->set_property("decodeAudioData", Value(decodeAudioData_fn.release()));
    
    // Destination node (speakers)
    auto destination = ObjectFactory::create_object();
    destination->set_property("__audio_node_type__", Value("destination"));
    destination->set_property("numberOfInputs", Value(1.0));
    destination->set_property("numberOfOutputs", Value(0.0));
    destination->set_property("channelCount", Value(2.0));
    context->set_property("destination", Value(destination.release()));
    
    std::cout << "🎵 Created Web Audio Context - Ready for EPIC sound processing!" << std::endl;
    return Value(context.release());
}

// 🎼 OSCILLATOR NODE - SYNTHESIZER POWER!
Value WebAPI::audio_createOscillator(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto oscillator = ObjectFactory::create_object();
    oscillator->set_property("__audio_node_type__", Value("oscillator"));
    oscillator->set_property("__node_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    // Oscillator properties
    oscillator->set_property("type", Value("sine"));
    oscillator->set_property("channelCount", Value(2.0));
    oscillator->set_property("numberOfInputs", Value(0.0));
    oscillator->set_property("numberOfOutputs", Value(1.0));
    
    // Frequency AudioParam
    auto frequency = ObjectFactory::create_object();
    frequency->set_property("value", Value(440.0)); // A4 note
    frequency->set_property("defaultValue", Value(440.0));
    frequency->set_property("minValue", Value(0.0));
    frequency->set_property("maxValue", Value(20000.0));
    
    auto setValueAtTime_fn = ObjectFactory::create_native_function("setValueAtTime", audioParam_setValueAtTime);
    frequency->set_property("setValueAtTime", Value(setValueAtTime_fn.release()));
    
    auto linearRampToValueAtTime_fn = ObjectFactory::create_native_function("linearRampToValueAtTime", audioParam_linearRampToValueAtTime);
    frequency->set_property("linearRampToValueAtTime", Value(linearRampToValueAtTime_fn.release()));
    
    oscillator->set_property("frequency", Value(frequency.release()));
    
    // Detune AudioParam
    auto detune = ObjectFactory::create_object();
    detune->set_property("value", Value(0.0));
    detune->set_property("defaultValue", Value(0.0));
    oscillator->set_property("detune", Value(detune.release()));
    
    // Oscillator methods
    auto connect_fn = ObjectFactory::create_native_function("connect", audioNode_connect);
    oscillator->set_property("connect", Value(connect_fn.release()));
    
    auto disconnect_fn = ObjectFactory::create_native_function("disconnect", audioNode_disconnect);
    oscillator->set_property("disconnect", Value(disconnect_fn.release()));
    
    auto start_fn = ObjectFactory::create_native_function("start", oscillator_start);
    oscillator->set_property("start", Value(start_fn.release()));
    
    auto stop_fn = ObjectFactory::create_native_function("stop", oscillator_stop);
    oscillator->set_property("stop", Value(stop_fn.release()));
    
    std::cout << "🎹 Created Oscillator Node - Wave generation READY! (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(oscillator.release());
}

// 🔊 GAIN NODE - VOLUME CONTROL MASTER!
Value WebAPI::audio_createGain(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto gainNode = ObjectFactory::create_object();
    gainNode->set_property("__audio_node_type__", Value("gain"));
    gainNode->set_property("__node_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    // Gain properties
    gainNode->set_property("channelCount", Value(2.0));
    gainNode->set_property("numberOfInputs", Value(1.0));
    gainNode->set_property("numberOfOutputs", Value(1.0));
    
    // Gain AudioParam
    auto gain = ObjectFactory::create_object();
    gain->set_property("value", Value(1.0));
    gain->set_property("defaultValue", Value(1.0));
    gain->set_property("minValue", Value(0.0));
    gain->set_property("maxValue", Value(10.0));
    
    auto setValueAtTime_fn = ObjectFactory::create_native_function("setValueAtTime", audioParam_setValueAtTime);
    gain->set_property("setValueAtTime", Value(setValueAtTime_fn.release()));
    
    auto linearRampToValueAtTime_fn = ObjectFactory::create_native_function("linearRampToValueAtTime", audioParam_linearRampToValueAtTime);
    gain->set_property("linearRampToValueAtTime", Value(linearRampToValueAtTime_fn.release()));
    
    gainNode->set_property("gain", Value(gain.release()));
    
    // Connection methods
    auto connect_fn = ObjectFactory::create_native_function("connect", audioNode_connect);
    gainNode->set_property("connect", Value(connect_fn.release()));
    
    auto disconnect_fn = ObjectFactory::create_native_function("disconnect", audioNode_disconnect);
    gainNode->set_property("disconnect", Value(disconnect_fn.release()));
    
    std::cout << "🔊 Created Gain Node - Volume control ACTIVATED! (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(gainNode.release());
}

// 📊 ANALYSER NODE - FREQUENCY ANALYSIS BEAST!
Value WebAPI::audio_createAnalyser(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto analyser = ObjectFactory::create_object();
    analyser->set_property("__audio_node_type__", Value("analyser"));
    analyser->set_property("__node_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    // Analyser properties
    analyser->set_property("fftSize", Value(2048.0));
    analyser->set_property("frequencyBinCount", Value(1024.0));
    analyser->set_property("minDecibels", Value(-100.0));
    analyser->set_property("maxDecibels", Value(-30.0));
    analyser->set_property("smoothingTimeConstant", Value(0.8));
    analyser->set_property("channelCount", Value(2.0));
    analyser->set_property("numberOfInputs", Value(1.0));
    analyser->set_property("numberOfOutputs", Value(1.0));
    
    // Analysis methods
    auto getByteFrequencyData_fn = ObjectFactory::create_native_function("getByteFrequencyData", analyserNode_getByteFrequencyData);
    analyser->set_property("getByteFrequencyData", Value(getByteFrequencyData_fn.release()));
    
    // Connection methods
    auto connect_fn = ObjectFactory::create_native_function("connect", audioNode_connect);
    analyser->set_property("connect", Value(connect_fn.release()));
    
    auto disconnect_fn = ObjectFactory::create_native_function("disconnect", audioNode_disconnect);
    analyser->set_property("disconnect", Value(disconnect_fn.release()));
    
    std::cout << "📊 Created Analyser Node - Frequency analysis READY! (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(analyser.release());
}

// 🎵 AUDIO BUFFER - SOUND DATA STORAGE!
Value WebAPI::audio_createBuffer(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 3) {
        std::cout << "createBuffer: Missing numberOfChannels, length, or sampleRate" << std::endl;
        return Value();
    }
    
    int numberOfChannels = static_cast<int>(args[0].to_number());
    int length = static_cast<int>(args[1].to_number());
    double sampleRate = args[2].to_number();
    
    auto buffer = ObjectFactory::create_object();
    buffer->set_property("__audio_buffer__", Value(true));
    buffer->set_property("numberOfChannels", Value(static_cast<double>(numberOfChannels)));
    buffer->set_property("length", Value(static_cast<double>(length)));
    buffer->set_property("sampleRate", Value(sampleRate));
    buffer->set_property("duration", Value(static_cast<double>(length) / sampleRate));
    
    std::cout << "🎵 Created Audio Buffer - " << numberOfChannels << " channels, " 
              << length << " samples at " << sampleRate << "Hz" << std::endl;
    return Value(buffer.release());
}

// 🎼 BUFFER SOURCE NODE - AUDIO PLAYBACK!
Value WebAPI::audio_createBufferSource(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    auto bufferSource = ObjectFactory::create_object();
    bufferSource->set_property("__audio_node_type__", Value("bufferSource"));
    bufferSource->set_property("__node_id__", Value(static_cast<double>(timer_id_counter_++)));
    
    // Buffer source properties
    bufferSource->set_property("buffer", Value());
    bufferSource->set_property("loop", Value(false));
    bufferSource->set_property("loopStart", Value(0.0));
    bufferSource->set_property("loopEnd", Value(0.0));
    bufferSource->set_property("channelCount", Value(2.0));
    bufferSource->set_property("numberOfInputs", Value(0.0));
    bufferSource->set_property("numberOfOutputs", Value(1.0));
    
    // Playback rate AudioParam
    auto playbackRate = ObjectFactory::create_object();
    playbackRate->set_property("value", Value(1.0));
    playbackRate->set_property("defaultValue", Value(1.0));
    bufferSource->set_property("playbackRate", Value(playbackRate.release()));
    
    // Methods
    auto connect_fn = ObjectFactory::create_native_function("connect", audioNode_connect);
    bufferSource->set_property("connect", Value(connect_fn.release()));
    
    auto disconnect_fn = ObjectFactory::create_native_function("disconnect", audioNode_disconnect);
    bufferSource->set_property("disconnect", Value(disconnect_fn.release()));
    
    auto start_fn = ObjectFactory::create_native_function("start", bufferSource_start);
    bufferSource->set_property("start", Value(start_fn.release()));
    
    std::cout << "🎼 Created Buffer Source Node - Audio playback READY! (ID: " << (timer_id_counter_ - 1) << ")" << std::endl;
    return Value(bufferSource.release());
}

// 🎧 AUDIO DATA DECODING - MP3/WAV PROCESSING!
Value WebAPI::audio_decodeAudioData(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "decodeAudioData: Missing audio data" << std::endl;
        return Value();
    }
    
    Value audioData = args[0];
    std::cout << "🎧 Decoding audio data (" << audioData.to_string().length() << " bytes) - Processing..." << std::endl;
    
    // Simulate decoding - return a Promise-like object
    auto decodedBuffer = ObjectFactory::create_object();
    decodedBuffer->set_property("numberOfChannels", Value(2.0));
    decodedBuffer->set_property("length", Value(44100.0)); // 1 second at 44.1kHz
    decodedBuffer->set_property("sampleRate", Value(44100.0));
    decodedBuffer->set_property("duration", Value(1.0));
    
    std::cout << "🎧 Audio data decoded successfully - 2 channels, 44.1kHz, 1.0s duration" << std::endl;
    return Value(decodedBuffer.release());
}

// 🔗 AUDIO NODE CONNECTION - SIGNAL ROUTING!
Value WebAPI::audioNode_connect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "connect: Missing destination node" << std::endl;
        return Value();
    }
    
    Object* thisNode = ctx.get_this_binding();
    Value destination = args[0];
    
    if (thisNode && destination.is_object()) {
        Value thisId = thisNode->get_property("__node_id__");
        Object* destObj = destination.as_object();
        Value destId = destObj->get_property("__node_id__");
        Value destType = destObj->get_property("__audio_node_type__");
        
        std::cout << "🔗 Connected audio node " << thisId.to_string() 
                  << " → " << destType.to_string() << " node " << destId.to_string() << std::endl;
    }
    
    return Value();
}

// ❌ AUDIO NODE DISCONNECTION
Value WebAPI::audioNode_disconnect(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    
    Object* thisNode = ctx.get_this_binding();
    if (thisNode) {
        Value thisId = thisNode->get_property("__node_id__");
        std::cout << "❌ Disconnected audio node " << thisId.to_string() << " from all destinations" << std::endl;
    }
    
    return Value();
}

// ▶️ OSCILLATOR START - SOUND GENERATION!
Value WebAPI::oscillator_start(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    
    double when = args.empty() ? 0.0 : args[0].to_number();
    
    Object* thisNode = ctx.get_this_binding();
    if (thisNode) {
        Value nodeId = thisNode->get_property("__node_id__");
        Value frequency = thisNode->get_property("frequency");
        
        if (frequency.is_object()) {
            Object* freqObj = frequency.as_object();
            Value freqValue = freqObj->get_property("value");
            std::cout << "▶️ Started oscillator " << nodeId.to_string() 
                      << " at " << freqValue.to_string() << "Hz (time: " << when << ")" << std::endl;
        }
    }
    
    return Value();
}

// ⏹️ OSCILLATOR STOP
Value WebAPI::oscillator_stop(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    
    double when = args.empty() ? 0.0 : args[0].to_number();
    
    Object* thisNode = ctx.get_this_binding();
    if (thisNode) {
        Value nodeId = thisNode->get_property("__node_id__");
        std::cout << "⏹️ Stopped oscillator " << nodeId.to_string() << " (time: " << when << ")" << std::endl;
    }
    
    return Value();
}

// 🎚️ AUDIO PARAM SET VALUE AT TIME
Value WebAPI::audioParam_setValueAtTime(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "setValueAtTime: Missing value or time" << std::endl;
        return Value();
    }
    
    double value = args[0].to_number();
    double time = args[1].to_number();
    
    Object* thisParam = ctx.get_this_binding();
    if (thisParam) {
        thisParam->set_property("value", Value(value));
        std::cout << "🎚️ Set audio param value to " << value << " at time " << time << std::endl;
    }
    
    return Value();
}

// 📈 AUDIO PARAM LINEAR RAMP
Value WebAPI::audioParam_linearRampToValueAtTime(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.size() < 2) {
        std::cout << "linearRampToValueAtTime: Missing value or time" << std::endl;
        return Value();
    }
    
    double value = args[0].to_number();
    double time = args[1].to_number();
    
    std::cout << "📈 Linear ramp audio param to " << value << " at time " << time << std::endl;
    return Value();
}

// 📊 FREQUENCY DATA ANALYSIS
Value WebAPI::analyserNode_getByteFrequencyData(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        std::cout << "getByteFrequencyData: Missing array" << std::endl;
        return Value();
    }
    
    Value array = args[0];
    if (array.is_object()) {
        Object* arrayObj = array.as_object();
        
        // Simulate frequency data (0-255 range)
        for (int i = 0; i < 1024; i++) {
            int freqValue = (i * 255) / 1024; // Simple gradient
            arrayObj->set_property(std::to_string(i), Value(static_cast<double>(freqValue)));
        }
        arrayObj->set_property("length", Value(1024.0));
        
        std::cout << "📊 Filled frequency data array with 1024 samples" << std::endl;
    }
    
    return Value();
}

// ▶️ BUFFER SOURCE START
Value WebAPI::bufferSource_start(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    
    double when = args.empty() ? 0.0 : args[0].to_number();
    double offset = args.size() > 1 ? args[1].to_number() : 0.0;
    double duration = args.size() > 2 ? args[2].to_number() : -1.0;
    
    Object* thisNode = ctx.get_this_binding();
    if (thisNode) {
        Value nodeId = thisNode->get_property("__node_id__");
        std::cout << "▶️ Started buffer source " << nodeId.to_string() 
                  << " at time " << when << " offset " << offset;
        if (duration > 0) {
            std::cout << " duration " << duration;
        }
        std::cout << std::endl;
    }
    
    return Value();
}

} // namespace Quanta