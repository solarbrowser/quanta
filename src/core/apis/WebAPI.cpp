/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
 * WebAPI Stub - Empty implementations
 * 
 * This file provides empty stub implementations of WebAPI methods
 * to maintain compilation compatibility while Web APIs are moved to the interface system.
 */

#include "quanta/core/apis/WebAPI.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/platform/NativeAPI.h"
#include <iostream>

namespace Quanta {

int WebAPI::timer_id_counter_ = 1;
std::vector<std::chrono::time_point<std::chrono::steady_clock>> WebAPI::timer_times_;

Value WebAPI::setTimeout(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: setTimeout called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value(timer_id_counter_++);
}

Value WebAPI::setInterval(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: setInterval called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value(timer_id_counter_++);
}

Value WebAPI::clearTimeout(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: clearTimeout called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value();
}

Value WebAPI::clearInterval(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: clearInterval called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value();
}

Value WebAPI::console_log(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: console.log called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].to_string();
    }
    std::cout << std::endl;
    return Value();
}

Value WebAPI::console_error(Context& ctx, const std::vector<Value>& args) {
    return console_log(ctx, args);
}

Value WebAPI::console_warn(Context& ctx, const std::vector<Value>& args) {
    return console_log(ctx, args);
}

Value WebAPI::console_info(Context& ctx, const std::vector<Value>& args) {
    return console_log(ctx, args);
}

Value WebAPI::console_debug(Context& ctx, const std::vector<Value>& args) {
    return console_log(ctx, args);
}

Value WebAPI::console_trace(Context& ctx, const std::vector<Value>& args) {
    return console_log(ctx, args);
}

Value WebAPI::console_time(Context& ctx, const std::vector<Value>& args) {
    return Value();
}

Value WebAPI::console_timeEnd(Context& ctx, const std::vector<Value>& args) {
    return Value();
}


Value WebAPI::document_getCookie(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: document.getCookie called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value("");
}

Value WebAPI::document_setCookie(Context& ctx, const std::vector<Value>& args) {
    std::cout << "WARNING: document.setCookie called but Web APIs not implemented. Use WebAPIInterface instead." << std::endl;
    return Value();
}

}
