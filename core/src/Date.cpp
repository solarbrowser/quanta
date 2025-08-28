/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Date.h"
#include <iostream>

namespace Quanta {

Date::Date() : time_point_(std::chrono::system_clock::now()) {}

Date::Date(int64_t timestamp) 
    : time_point_(std::chrono::system_clock::from_time_t(timestamp / 1000)) {
    // JavaScript timestamps are in milliseconds, C++ uses seconds
}

Date::Date(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    std::tm tm = {};
    tm.tm_year = year - 1900; // tm_year is years since 1900
    tm.tm_mon = month - 1;    // tm_mon is 0-11
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    
    std::time_t time = std::mktime(&tm);
    time_point_ = std::chrono::system_clock::from_time_t(time);
    
    // Add milliseconds
    time_point_ += std::chrono::milliseconds(millisecond);
}

// Static methods
Value Date::now(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Suppress unused warnings
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return Value(static_cast<double>(timestamp));
}

Value Date::parse(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    std::string date_str = args[0].to_string();
    // Simple ISO 8601 parsing (YYYY-MM-DD format)
    if (date_str.length() >= 10) {
        try {
            int year = std::stoi(date_str.substr(0, 4));
            int month = std::stoi(date_str.substr(5, 2));
            int day = std::stoi(date_str.substr(8, 2));
            
            Date date(year, month, day);
            return Value(static_cast<double>(date.getTimestamp()));
        } catch (...) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
    }
    
    return Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::UTC(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    if (args.size() < 2) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    int year = static_cast<int>(args[0].to_number());
    int month = static_cast<int>(args[1].to_number());
    int day = args.size() > 2 ? static_cast<int>(args[2].to_number()) : 1;
    int hour = args.size() > 3 ? static_cast<int>(args[3].to_number()) : 0;
    int minute = args.size() > 4 ? static_cast<int>(args[4].to_number()) : 0;
    int second = args.size() > 5 ? static_cast<int>(args[5].to_number()) : 0;
    int millisecond = args.size() > 6 ? static_cast<int>(args[6].to_number()) : 0;
    
    Date date(year, month + 1, day, hour, minute, second, millisecond); // month is 0-based in JS
    return Value(static_cast<double>(date.getTimestamp()));
}

// Instance methods (these would be called on Date objects)
Value Date::getTime(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Suppress unused warnings
    // In a full implementation, 'this' would be the Date object
    Date date; // For now, use current time
    return Value(static_cast<double>(date.getTimestamp()));
}

Value Date::getFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_year + 1900));
}

Value Date::getMonth(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_mon)); // 0-based
}

Value Date::getDate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_mday));
}

Value Date::getDay(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_wday)); // 0 = Sunday
}

Value Date::getHours(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_hour));
}

Value Date::getMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_min));
}

Value Date::getSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_sec));
}

Value Date::getMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        date.time_point_.time_since_epoch()).count() % 1000;
    return Value(static_cast<double>(ms));
}

// Setters (simplified implementations)
Value Date::setTime(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return args[0]; // Return the timestamp
}

Value Date::setFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setMonth(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setDate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setHours(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

Value Date::setMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return Date::getTime(ctx, args);
}

// String methods
Value Date::toString(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::time_t time = date.getTimeT();
    std::string time_str = std::ctime(&time);
    // Remove newline at the end
    if (!time_str.empty() && time_str.back() == '\n') {
        time_str.pop_back();
    }
    return Value(time_str);
}

Value Date::toISOString(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::tm utc_time = date.getUTCTime();
    
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (utc_time.tm_year + 1900) << "-"
        << std::setw(2) << (utc_time.tm_mon + 1) << "-"
        << std::setw(2) << utc_time.tm_mday << "T"
        << std::setw(2) << utc_time.tm_hour << ":"
        << std::setw(2) << utc_time.tm_min << ":"
        << std::setw(2) << utc_time.tm_sec << ".000Z";
    
    return Value(oss.str());
}

Value Date::toJSON(Context& ctx, const std::vector<Value>& args) {
    return toISOString(ctx, args); // JSON representation is ISO string
}

// Utility methods
int64_t Date::getTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        time_point_.time_since_epoch()).count();
}

std::time_t Date::getTimeT() const {
    return std::chrono::system_clock::to_time_t(time_point_);
}

std::tm Date::getLocalTime() const {
    std::time_t time = getTimeT();
    std::tm* local_time = std::localtime(&time);
    return local_time ? *local_time : std::tm{};
}

std::tm Date::getUTCTime() const {
    std::time_t time = getTimeT();
    std::tm* utc_time = std::gmtime(&time);
    return utc_time ? *utc_time : std::tm{};
}

Value Date::date_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning
    
    std::unique_ptr<Date> date_impl;
    
    if (args.empty()) {
        // new Date() - current time
        date_impl = std::make_unique<Date>();
    } else if (args.size() == 1) {
        // new Date(timestamp) or new Date(string)
        if (args[0].is_number()) {
            int64_t timestamp = static_cast<int64_t>(args[0].to_number());
            date_impl = std::make_unique<Date>(timestamp);
        } else {
            // Parse string - for now, just return current time
            date_impl = std::make_unique<Date>();
        }
    } else {
        // new Date(year, month, day, ...)
        int year = static_cast<int>(args[0].to_number());
        int month = static_cast<int>(args[1].to_number());
        int day = args.size() > 2 ? static_cast<int>(args[2].to_number()) : 1;
        int hour = args.size() > 3 ? static_cast<int>(args[3].to_number()) : 0;
        int minute = args.size() > 4 ? static_cast<int>(args[4].to_number()) : 0;
        int second = args.size() > 5 ? static_cast<int>(args[5].to_number()) : 0;
        int millisecond = args.size() > 6 ? static_cast<int>(args[6].to_number()) : 0;
        
        date_impl = std::make_unique<Date>(year, month, day, hour, minute, second, millisecond);
    }
    
    // Create a simple Date object wrapper
    auto js_date_obj = ObjectFactory::create_object();
    
    // Add standard properties
    js_date_obj->set_property("_isDate", Value(true));
    
    // Store the timestamp as a property that can be accessed by instance methods
    js_date_obj->set_property("_timestamp", Value(static_cast<double>(date_impl->getTimestamp())));
    
    return Value(js_date_obj.release());
}

} // namespace Quanta