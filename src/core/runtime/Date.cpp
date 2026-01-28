/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Date.h"
#include <iostream>

namespace Quanta {

Date::Date() : time_point_(std::chrono::system_clock::now()), is_invalid_(false) {}

Date::Date(int64_t timestamp) {
    if (timestamp == LLONG_MIN) {
        is_invalid_ = true;
        time_point_ = std::chrono::system_clock::now();
    } else {
        is_invalid_ = false;
        // Use milliseconds directly instead of converting through time_t
        auto duration = std::chrono::milliseconds(timestamp);
        time_point_ = std::chrono::system_clock::time_point(duration);
    }
}

Date::Date(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    is_invalid_ = false;

    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;

    std::time_t time = std::mktime(&tm);
    time_point_ = std::chrono::system_clock::from_time_t(time);

    time_point_ += std::chrono::milliseconds(millisecond);
}

Value Date::now(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return Value(static_cast<double>(timestamp));
}

Value Date::parse(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    std::string date_str = args[0].to_string();
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
    (void)ctx;
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

    // Use UTC time (tm_year is years since 1900, tm_mon is 0-11)
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;  
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;

    // Convert UTC time to timestamp
#ifdef _WIN32
    std::time_t time = _mkgmtime(&tm);
#else
    std::time_t time = timegm(&tm);
#endif
    if (time == -1) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    int64_t timestamp_ms = static_cast<int64_t>(time) * 1000 + millisecond;
    return Value(static_cast<double>(timestamp_ms));
}

Value Date::getTime(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    return timestamp_val;
}

Value Date::getFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_year + 1900));
}

Value Date::getMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_mon));
}

Value Date::getDate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_mday));
}

Value Date::getDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_wday));
}

Value Date::getHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_hour));
}

Value Date::getMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_min));
}

Value Date::getSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(static_cast<double>(local_tm->tm_sec));
}

Value Date::getMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    return Value(static_cast<double>(ms));
}

Value Date::setTime(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double time_val = args[0].to_number();
    date_obj->set_property("_timestamp", Value(time_val));
    return Value(time_val);
}

Value Date::setFullYear(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year_val = args[0].to_number();
    if (std::isnan(year_val) || std::isinf(year_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_year = static_cast<int>(year_val) - 1900;

    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setMonth(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double month_val = args[0].to_number();
    if (std::isnan(month_val) || std::isinf(month_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_mon = static_cast<int>(month_val);

    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setDate(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Get the date value
    double date_val = args[0].to_number();
    if (std::isnan(date_val) || std::isinf(date_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Convert timestamp to local time
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Set the date
    local_tm->tm_mday = static_cast<int>(date_val);

    // Convert back to timestamp
    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;

    // Get milliseconds from original timestamp and add them
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    // Update the timestamp
    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setHours(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double hours_val = args[0].to_number();
    if (std::isnan(hours_val) || std::isinf(hours_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_hour = static_cast<int>(hours_val);

    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setMinutes(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double minutes_val = args[0].to_number();
    if (std::isnan(minutes_val) || std::isinf(minutes_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_min = static_cast<int>(minutes_val);

    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setSeconds(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double seconds_val = args[0].to_number();
    if (std::isnan(seconds_val) || std::isinf(seconds_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_sec = static_cast<int>(seconds_val);

    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setMilliseconds(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double ms_val = args[0].to_number();
    if (std::isnan(ms_val) || std::isinf(ms_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Remove old milliseconds and add new ones
    double new_timestamp = std::floor(timestamp / 1000.0) * 1000.0 + ms_val;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::toString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::string("Invalid Date"));
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::string("Invalid Date"));
    }
    std::time_t time = static_cast<std::time_t>(timestamp / 1000);
    std::string time_str = std::ctime(&time);
    if (!time_str.empty() && time_str.back() == '\n') {
        time_str.pop_back();
    }
    return Value(time_str);
}

Value Date::toISOString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        ctx.throw_range_error("Invalid Date");
        return Value();
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();

    // If timestamp is NaN or infinite, throw RangeError
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        ctx.throw_range_error("Invalid Date");
        return Value();
    }

    // Convert timestamp to UTC time
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_time = std::gmtime(&tt);

    if (!utc_time) {
        ctx.throw_range_error("Invalid Date");
        return Value();
    }

    // Get milliseconds
    double ms_part = std::fmod(timestamp, 1000.0);
    if (ms_part < 0) ms_part += 1000.0;
    int milliseconds = static_cast<int>(ms_part);

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (utc_time->tm_year + 1900) << "-"
        << std::setw(2) << (utc_time->tm_mon + 1) << "-"
        << std::setw(2) << utc_time->tm_mday << "T"
        << std::setw(2) << utc_time->tm_hour << ":"
        << std::setw(2) << utc_time->tm_min << ":"
        << std::setw(2) << utc_time->tm_sec << "."
        << std::setw(3) << milliseconds << "Z";

    return Value(oss.str());
}

Value Date::toJSON(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value::null();
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();

    // If timestamp is NaN or infinite, return null (not throw error)
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value::null();
    }

    // Convert timestamp to UTC time
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_time = std::gmtime(&tt);

    if (!utc_time) {
        return Value::null();
    }

    // Get milliseconds
    double ms_part = std::fmod(timestamp, 1000.0);
    if (ms_part < 0) ms_part += 1000.0;
    int milliseconds = static_cast<int>(ms_part);

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (utc_time->tm_year + 1900) << "-"
        << std::setw(2) << (utc_time->tm_mon + 1) << "-"
        << std::setw(2) << utc_time->tm_mday << "T"
        << std::setw(2) << utc_time->tm_hour << ":"
        << std::setw(2) << utc_time->tm_min << ":"
        << std::setw(2) << utc_time->tm_sec << "."
        << std::setw(3) << milliseconds << "Z";

    return Value(oss.str());
}

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
    (void)ctx;
    
    std::unique_ptr<Date> date_impl;
    
    if (args.empty()) {
        date_impl = std::make_unique<Date>();
    } else if (args.size() == 1) {
        double timestamp = args[0].to_number();
        if (std::isnan(timestamp) || std::isinf(timestamp)) {
            date_impl = std::make_unique<Date>(LLONG_MIN);
        } else {
            date_impl = std::make_unique<Date>(static_cast<int64_t>(timestamp));
        }
    } else {
        int year = static_cast<int>(args[0].to_number());
        int month = static_cast<int>(args[1].to_number());
        int day = args.size() > 2 ? static_cast<int>(args[2].to_number()) : 1;
        int hour = args.size() > 3 ? static_cast<int>(args[3].to_number()) : 0;
        int minute = args.size() > 4 ? static_cast<int>(args[4].to_number()) : 0;
        int second = args.size() > 5 ? static_cast<int>(args[5].to_number()) : 0;
        int millisecond = args.size() > 6 ? static_cast<int>(args[6].to_number()) : 0;
        
        date_impl = std::make_unique<Date>(year, month, day, hour, minute, second, millisecond);
    }
    
    auto js_date_obj = ObjectFactory::create_object();
    
    js_date_obj->set_property("_isDate", Value(true));
    
    js_date_obj->set_property("_timestamp", Value(date_impl->getTimestamp()));
    
    return Value(js_date_obj.release());
}

Value Date::getYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (!date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();

    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    return Value(static_cast<double>(local_tm->tm_year));
}

Value Date::setYear(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year_value = args[0].to_number();
    if (std::isnan(year_value) || std::isinf(year_value)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    int year = static_cast<int>(year_value);

    // ES1 B.2.5: If year is 0-99, add 1900
    if (year >= 0 && year <= 99) {
        year += 1900;
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* local_tm = std::localtime(&tt);
    if (!local_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    local_tm->tm_year = year - 1900;
    std::time_t new_tt = std::mktime(local_tm);
    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::getTimezoneOffset(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&tt);
    std::tm* utc_tm = std::gmtime(&tt);

    if (!local_tm || !utc_tm) {
        return Value(0.0);
    }

    int offset_hours = local_tm->tm_hour - utc_tm->tm_hour;
    int offset_mins = local_tm->tm_min - utc_tm->tm_min;

    int total_offset = -(offset_hours * 60 + offset_mins);

    return Value(static_cast<double>(total_offset));
}

Value Date::getUTCDate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_mday)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_wday)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_year + 1900)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_hour)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    return Value(static_cast<double>(ms));
}

Value Date::getUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_min)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_mon)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    return utc_tm ? Value(static_cast<double>(utc_tm->tm_sec)) : Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::setUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year_val = args[0].to_number();
    if (std::isnan(year_val) || std::isinf(year_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_year = static_cast<int>(year_val) - 1900;

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCMonth(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double month_val = args[0].to_number();
    if (std::isnan(month_val) || std::isinf(month_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_mon = static_cast<int>(month_val);

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCDate(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double date_val = args[0].to_number();
    if (std::isnan(date_val) || std::isinf(date_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_mday = static_cast<int>(date_val);

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCHours(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double hours_val = args[0].to_number();
    if (std::isnan(hours_val) || std::isinf(hours_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_hour = static_cast<int>(hours_val);

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double minutes_val = args[0].to_number();
    if (std::isnan(minutes_val) || std::isinf(minutes_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_min = static_cast<int>(minutes_val);

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double seconds_val = args[0].to_number();
    if (std::isnan(seconds_val) || std::isinf(seconds_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::time_t tt = static_cast<std::time_t>(timestamp / 1000);
    std::tm* utc_tm = std::gmtime(&tt);
    if (!utc_tm) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    std::tm tm_copy = *utc_tm;
    tm_copy.tm_sec = static_cast<int>(seconds_val);

    #ifdef _WIN32
    std::time_t new_tt = _mkgmtime(&tm_copy);
    #else
    std::time_t new_tt = timegm(&tm_copy);
    #endif

    double new_timestamp = static_cast<double>(new_tt) * 1000.0;
    int64_t ms = static_cast<int64_t>(timestamp) % 1000;
    new_timestamp += ms;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

Value Date::setUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());

    Object* date_obj = ctx.get_this_binding();
    if (!date_obj || !date_obj->has_property("_timestamp")) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    Value timestamp_val = date_obj->get_property("_timestamp");
    double timestamp = timestamp_val.to_number();
    if (std::isnan(timestamp) || std::isinf(timestamp)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double ms_val = args[0].to_number();
    if (std::isnan(ms_val) || std::isinf(ms_val)) {
        date_obj->set_property("_timestamp", Value(std::numeric_limits<double>::quiet_NaN()));
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Remove old milliseconds and add new ones
    double new_timestamp = std::floor(timestamp / 1000.0) * 1000.0 + ms_val;

    date_obj->set_property("_timestamp", Value(new_timestamp));
    return Value(new_timestamp);
}

}
