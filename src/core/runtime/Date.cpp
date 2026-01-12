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
        time_point_ = std::chrono::system_clock::from_time_t(timestamp / 1000);
    }
}

Date::Date(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    is_invalid_ = false;

    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;  // month is already 0-based in JavaScript
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
            int month = std::stoi(date_str.substr(5, 2)) - 1;  // ISO 8601 months are 1-based
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

    Date date(year, month, day, hour, minute, second, millisecond);
    return Value(static_cast<double>(date.getTimestamp()));
}

Value Date::getTime(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (timestamp.is_number()) {
        return timestamp;
    }
    return Value(std::numeric_limits<double>::quiet_NaN());
}

Value Date::getFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_year + 1900));
}

Value Date::getMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_mon));
}

Value Date::getDate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_mday));
}

Value Date::getDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_wday));
}

Value Date::getHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_hour));
}

Value Date::getMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_min));
}

Value Date::getSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm local_time = date.getLocalTime();
    return Value(static_cast<double>(local_time.tm_sec));
}

Value Date::getMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        date.time_point_.time_since_epoch()).count() % 1000;
    return Value(static_cast<double>(ms));
}

Value Date::setTime(Context& ctx, const std::vector<Value>& args) {
    (void)ctx;
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    return args[0];
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

Value Date::toString(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    Date date;
    std::time_t time = date.getTimeT();
    std::string time_str = std::ctime(&time);
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
    return toISOString(ctx, args);
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
    (void)ctx;

    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    double year_value = args[0].to_number();
    if (std::isnan(year_value) || std::isinf(year_value)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }

    int year = static_cast<int>(year_value);

    if (year >= 0 && year <= 99) {
        year += 1900;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&tt);

    local_tm->tm_year = year - 1900;
    std::time_t new_time = std::mktime(local_tm);

    auto new_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::from_time_t(new_time).time_since_epoch()).count();

    return Value(static_cast<double>(new_timestamp));
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
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_mday));
}

Value Date::getUTCDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_wday));
}

Value Date::getUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_year + 1900));
}

Value Date::getUTCHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_hour));
}

Value Date::getUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        date.time_point_.time_since_epoch()).count();
    return Value(static_cast<double>(ms % 1000));
}

Value Date::getUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_min));
}

Value Date::getUTCMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_mon));
}

Value Date::getUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) return Value(std::numeric_limits<double>::quiet_NaN());

    Value timestamp = this_obj->get_property("_timestamp");
    if (!timestamp.is_number()) return Value(std::numeric_limits<double>::quiet_NaN());

    Date date(static_cast<int64_t>(timestamp.as_number()));
    std::tm utc_time = date.getUTCTime();
    return Value(static_cast<double>(utc_time.tm_sec));
}

Value Date::setUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
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

}
