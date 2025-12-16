/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Date.h"
#include <iostream>

namespace Quanta {

Date::Date() : time_point_(std::chrono::system_clock::now()), is_invalid_(false) {}

Date::Date(int64_t timestamp) {
    // Check if timestamp represents invalid date
    if (timestamp == LLONG_MIN) {
        is_invalid_ = true;
        time_point_ = std::chrono::system_clock::now(); // Dummy value
    } else {
        is_invalid_ = false;
        time_point_ = std::chrono::system_clock::from_time_t(timestamp / 1000);
    }
}

Date::Date(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    is_invalid_ = false; // Valid dates by default

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
    Date date; // For now, use current time - this needs proper 'this' binding
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
double Date::getTimestamp() const {
    if (is_invalid_) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        time_point_.time_since_epoch()).count());
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
        // Always convert to number first - objects will become NaN
        double timestamp = args[0].to_number();
        if (std::isnan(timestamp) || std::isinf(timestamp)) {
            // Create invalid date with special timestamp
            date_impl = std::make_unique<Date>(LLONG_MIN); // Special value for invalid dates
        } else {
            date_impl = std::make_unique<Date>(static_cast<int64_t>(timestamp));
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
    js_date_obj->set_property("_timestamp", Value(date_impl->getTimestamp()));
    
    return Value(js_date_obj.release());
}

// Legacy methods (Annex B)
Value Date::getYear(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Suppress unused warnings

    // Check if this is a valid Date object
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (!date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Get stored timestamp
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();

    // If timestamp is NaN, return NaN
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Convert timestamp to time_t and get year
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000); // Convert ms to seconds
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    return Value(static_cast<double>(local_tm->tm_year)); // tm_year is already year - 1900
}

Value Date::setYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused warning

    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year_value = args[0].to_number();
    if (std::isnan(year_value) || std::isinf(year_value)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    int year = static_cast<int>(year_value);

    // setYear behavior: if year is 0-99, add 1900; otherwise use as-is
    if (year >= 0 && year <= 99) {
        year += 1900;
    }

    // For now, return the set year as timestamp
    // This is a simplified implementation
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&tt);

    local_tm->tm_year = year - 1900;
    std::time_t new_time = std::mktime(local_tm);

    auto new_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::from_time_t(new_time).time_since_epoch()).count();

    return Value(static_cast<double>(new_timestamp));
}

// Get timezone offset in minutes
Value Date::getTimezoneOffset(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&tt);
    std::tm* utc_tm = std::gmtime(&tt);

    if (!local_tm || !utc_tm) {
        return Value(0.0);
    }

    // Calculate offset in minutes
    int offset_hours = local_tm->tm_hour - utc_tm->tm_hour;
    int offset_mins = local_tm->tm_min - utc_tm->tm_min;

    // JavaScript returns offset in minutes (negative for ahead of UTC)
    int total_offset = -(offset_hours * 60 + offset_mins);

    return Value(static_cast<double>(total_offset));
}

// UTC Date methods
Value Date::getUTCDate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_mday)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCDay(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_wday)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_year + 1900)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCHours(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_hour)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms % 1000));
}

Value Date::getUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_min)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCMonth(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_mon)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_sec)) : Value(std::numeric_limits<double>::quiet_NaN());
}

// UTC Setters - simplified implementations
Value Date::setUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    // Simplified: return current time
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCMonth(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCDate(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCHours(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

Value Date::setUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return Value(static_cast<double>(ms));
}

} // namespace Quanta