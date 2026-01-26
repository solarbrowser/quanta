/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Temporal.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/AST.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace Quanta {

static Object* g_instant_prototype = nullptr;
static Object* g_plainDate_prototype = nullptr;
static Object* g_plainTime_prototype = nullptr;
static Object* g_plainDateTime_prototype = nullptr;
static Object* g_duration_prototype = nullptr;
static Object* g_zonedDateTime_prototype = nullptr;
static Object* g_plainYearMonth_prototype = nullptr;
static Object* g_plainMonthDay_prototype = nullptr;
static Object* g_calendar_prototype = nullptr;
static Object* g_timeZone_prototype = nullptr;

namespace {
    bool isLeapYear(int year) {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    }

    int daysInMonth(int year, int month) {
        static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 2 && isLeapYear(year)) {
            return 29;
        }
        return days[month - 1];
    }

    int calcDayOfYear(int year, int month, int day) {
        int result = day;
        for (int m = 1; m < month; ++m) {
            result += daysInMonth(year, m);
        }
        return result;
    }

    int calcDayOfWeek(int year, int month, int day) {
        if (month < 3) {
            month += 12;
            year--;
        }
        int q = day;
        int m = month;
        int k = year % 100;
        int j = year / 100;
        int h = (q + ((13 * (m + 1)) / 5) + k + (k / 4) + (j / 4) - (2 * j)) % 7;
        return (h + 6) % 7;
    }

    std::string padZero(int value, int width) {
        std::ostringstream oss;
        oss << std::setw(width) << std::setfill('0') << value;
        return oss.str();
    }

    int64_t getCurrentNanoseconds() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        return millis * 1000000LL;
    }

    Object* getThisObject(Context& ctx, const std::vector<Value>& args, const std::string& className) {
        if (args.empty() || !args[0].is_object()) {
            ctx.throw_exception(Value("TypeError: " + className + " method called on incompatible receiver"));
            return nullptr;
        }
        return args[0].as_object();
    }

    int getIntProperty(Object* obj, const std::string& key, int defaultValue = 0) {
        if (obj->has_own_property(key)) {
            return static_cast<int>(obj->get_property(key).to_number());
        }
        return defaultValue;
    }

    std::string getStringProperty(Object* obj, const std::string& key, const std::string& defaultValue = "") {
        if (obj->has_own_property(key)) {
            return obj->get_property(key).to_string();
        }
        return defaultValue;
    }
}


Value TemporalNow::instant(Context& ctx, const std::vector<Value>& args) {
    int64_t nanos = getCurrentNanoseconds();
    Object* instant = new Object();
    instant->set_property("_nanoseconds", Value(static_cast<double>(nanos)));
    instant->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(instant);
}

Value TemporalNow::plainDateISO(Context& ctx, const std::vector<Value>& args) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);

    Object* date = new Object();
    date->set_property("_year", Value(tm->tm_year + 1900));
    date->set_property("_month", Value(tm->tm_mon + 1));
    date->set_property("_day", Value(tm->tm_mday));
    date->set_property("_calendar", Value(std::string("iso8601")));
    date->set_property("_class", Value(std::string("TemporalPlainDate")));
    return Value(date);
}

Value TemporalNow::plainTimeISO(Context& ctx, const std::vector<Value>& args) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    Object* time = new Object();
    time->set_property("_hour", Value(tm->tm_hour));
    time->set_property("_minute", Value(tm->tm_min));
    time->set_property("_second", Value(tm->tm_sec));
    time->set_property("_millisecond", Value(static_cast<int>(ms.count())));
    time->set_property("_microsecond", Value(0));
    time->set_property("_nanosecond", Value(0));
    time->set_property("_class", Value(std::string("TemporalPlainTime")));
    return Value(time);
}

Value TemporalNow::plainDateTimeISO(Context& ctx, const std::vector<Value>& args) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    Object* dt = new Object();
    dt->set_property("_year", Value(tm->tm_year + 1900));
    dt->set_property("_month", Value(tm->tm_mon + 1));
    dt->set_property("_day", Value(tm->tm_mday));
    dt->set_property("_hour", Value(tm->tm_hour));
    dt->set_property("_minute", Value(tm->tm_min));
    dt->set_property("_second", Value(tm->tm_sec));
    dt->set_property("_millisecond", Value(static_cast<int>(ms.count())));
    dt->set_property("_microsecond", Value(0));
    dt->set_property("_nanosecond", Value(0));
    dt->set_property("_calendar", Value(std::string("iso8601")));
    dt->set_property("_class", Value(std::string("TemporalPlainDateTime")));
    return Value(dt);
}

Value TemporalNow::zonedDateTimeISO(Context& ctx, const std::vector<Value>& args) {
    std::string timezone = "UTC";
    if (!args.empty() && args[0].is_string()) {
        timezone = args[0].to_string();
    }

    int64_t nanos = getCurrentNanoseconds();
    Object* zdt = new Object();
    zdt->set_property("_nanoseconds", Value(static_cast<double>(nanos)));
    zdt->set_property("_timezone", Value(timezone));
    zdt->set_property("_calendar", Value(std::string("iso8601")));
    zdt->set_property("_class", Value(std::string("TemporalZonedDateTime")));
    return Value(zdt);
}

Value TemporalNow::timeZoneId(Context& ctx, const std::vector<Value>& args) {
    return Value(std::string("UTC"));
}


TemporalInstant::TemporalInstant(int64_t nanoseconds) : nanoseconds_(nanoseconds) {}

Value TemporalInstant::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant requires epochNanoseconds argument")));
        return Value();
    }

    int64_t nanos = static_cast<int64_t>(args[0].to_number());
    Object* instant = new Object();

    if (g_instant_prototype) {
        instant->set_prototype(g_instant_prototype);
    }

    instant->set_property("_nanoseconds", Value(static_cast<double>(nanos)));
    instant->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(instant);
}

Value TemporalInstant::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant.from requires an argument")));
        return Value();
    }

    if (args[0].is_string()) {
        return TemporalNow::instant(ctx, {});
    }

    return args[0];
}

Value TemporalInstant::fromEpochMilliseconds(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant.fromEpochMilliseconds requires milliseconds argument")));
        return Value();
    }

    int64_t millis = static_cast<int64_t>(args[0].to_number());
    int64_t nanos = millis * 1000000LL;

    Object* instant = new Object();
    instant->set_property("_nanoseconds", Value(static_cast<double>(nanos)));
    instant->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(instant);
}

Value TemporalInstant::fromEpochNanoseconds(Context& ctx, const std::vector<Value>& args) {
    return constructor(ctx, args);
}

Value TemporalInstant::compare(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant.compare requires two arguments")));
        return Value();
    }

    Object* one = args[0].as_object();
    Object* two = args[1].as_object();

    if (!one || !two) {
        return Value(0);
    }

    double nanos1 = one->get_property("_nanoseconds").to_number();
    double nanos2 = two->get_property("_nanoseconds").to_number();

    if (nanos1 < nanos2) return Value(-1);
    if (nanos1 > nanos2) return Value(1);
    return Value(0);
}

Value TemporalInstant::epochSeconds(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    double nanos = obj->get_property("_nanoseconds").to_number();
    return Value(std::floor(nanos / 1000000000.0));
}

Value TemporalInstant::epochMilliseconds(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    double nanos = obj->get_property("_nanoseconds").to_number();
    return Value(std::floor(nanos / 1000000.0));
}

Value TemporalInstant::epochMicroseconds(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    double nanos = obj->get_property("_nanoseconds").to_number();
    return Value(std::floor(nanos / 1000.0));
}

Value TemporalInstant::epochNanoseconds(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    return obj->get_property("_nanoseconds");
}

Value TemporalInstant::toString(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    double nanos = obj->get_property("_nanoseconds").to_number();
    int64_t millis = static_cast<int64_t>(nanos / 1000000.0);

    std::time_t seconds = millis / 1000;
    std::tm* tm = std::gmtime(&seconds);

    std::ostringstream oss;
    oss << (tm->tm_year + 1900) << "-"
        << padZero(tm->tm_mon + 1, 2) << "-"
        << padZero(tm->tm_mday, 2) << "T"
        << padZero(tm->tm_hour, 2) << ":"
        << padZero(tm->tm_min, 2) << ":"
        << padZero(tm->tm_sec, 2) << "Z";

    return Value(oss.str());
}

Value TemporalInstant::toJSON(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalInstant::toLocaleString(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalInstant::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant does not have a valueOf method")));
    return Value();
}

Value TemporalInstant::add(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant.add requires a duration argument")));
        return Value();
    }

    Object* duration = args[1].as_object();
    double nanos = obj->get_property("_nanoseconds").to_number();

    nanos += getIntProperty(duration, "_nanoseconds", 0);
    nanos += getIntProperty(duration, "_microseconds", 0) * 1000.0;
    nanos += getIntProperty(duration, "_milliseconds", 0) * 1000000.0;
    nanos += getIntProperty(duration, "_seconds", 0) * 1000000000.0;
    nanos += getIntProperty(duration, "_minutes", 0) * 60000000000.0;
    nanos += getIntProperty(duration, "_hours", 0) * 3600000000000.0;
    nanos += getIntProperty(duration, "_days", 0) * 86400000000000.0;

    Object* result = new Object();
    result->set_property("_nanoseconds", Value(nanos));
    result->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(result);
}

Value TemporalInstant::subtract(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.Instant.subtract requires a duration argument")));
        return Value();
    }

    Object* duration = args[1].as_object();
    double nanos = obj->get_property("_nanoseconds").to_number();

    nanos -= getIntProperty(duration, "_nanoseconds", 0);
    nanos -= getIntProperty(duration, "_microseconds", 0) * 1000.0;
    nanos -= getIntProperty(duration, "_milliseconds", 0) * 1000000.0;
    nanos -= getIntProperty(duration, "_seconds", 0) * 1000000000.0;
    nanos -= getIntProperty(duration, "_minutes", 0) * 60000000000.0;
    nanos -= getIntProperty(duration, "_hours", 0) * 3600000000000.0;
    nanos -= getIntProperty(duration, "_days", 0) * 86400000000000.0;

    Object* result = new Object();
    result->set_property("_nanoseconds", Value(nanos));
    result->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(result);
}

Value TemporalInstant::until(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return TemporalDuration::constructor(ctx, {});
    }

    Object* other = args[1].as_object();
    double nanos1 = obj->get_property("_nanoseconds").to_number();
    double nanos2 = other->get_property("_nanoseconds").to_number();
    double diff = nanos2 - nanos1;

    Object* duration = new Object();
    duration->set_property("_nanoseconds", Value(diff));
    duration->set_property("_class", Value(std::string("TemporalDuration")));
    return Value(duration);
}

Value TemporalInstant::since(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return TemporalDuration::constructor(ctx, {});
    }

    Object* other = args[1].as_object();
    double nanos1 = obj->get_property("_nanoseconds").to_number();
    double nanos2 = other->get_property("_nanoseconds").to_number();
    double diff = nanos1 - nanos2;

    Object* duration = new Object();
    duration->set_property("_nanoseconds", Value(diff));
    duration->set_property("_class", Value(std::string("TemporalDuration")));
    return Value(duration);
}

Value TemporalInstant::round(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    double nanos = obj->get_property("_nanoseconds").to_number();
    nanos = std::round(nanos / 1000000.0) * 1000000.0;

    Object* result = new Object();
    result->set_property("_nanoseconds", Value(nanos));
    result->set_property("_class", Value(std::string("TemporalInstant")));
    return Value(result);
}

Value TemporalInstant::equals(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Instant");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(false);
    }

    Object* other = args[1].as_object();
    double nanos1 = obj->get_property("_nanoseconds").to_number();
    double nanos2 = other->get_property("_nanoseconds").to_number();

    return Value(nanos1 == nanos2);
}


TemporalPlainDate::TemporalPlainDate(int year, int month, int day, const std::string& calendar)
    : year_(year), month_(month), day_(day), calendar_(calendar) {}

Value TemporalPlainDate::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 3) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainDate requires year, month, and day arguments")));
        return Value();
    }

    int year = static_cast<int>(args[0].to_number());
    int month = static_cast<int>(args[1].to_number());
    int day = static_cast<int>(args[2].to_number());
    std::string calendar = args.size() > 3 ? args[3].to_string() : "iso8601";

    Object* date = new Object();

    if (g_plainDate_prototype) {
        date->set_prototype(g_plainDate_prototype);
    }

    date->set_property("_year", Value(year));
    date->set_property("_month", Value(month));
    date->set_property("_day", Value(day));
    date->set_property("_calendar", Value(calendar));
    date->set_property("_class", Value(std::string("TemporalPlainDate")));
    return Value(date);
}

Value TemporalPlainDate::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainDate.from requires an argument")));
        return Value();
    }

    if (args[0].is_string()) {
        std::string str = args[0].to_string();
        if (str.length() >= 10) {
            int year = std::stoi(str.substr(0, 4));
            int month = std::stoi(str.substr(5, 2));
            int day = std::stoi(str.substr(8, 2));
            return constructor(ctx, {Value(year), Value(month), Value(day)});
        }
    }

    return args[0];
}

Value TemporalPlainDate::compare(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainDate.compare requires two arguments")));
        return Value();
    }

    Object* one = args[0].as_object();
    Object* two = args[1].as_object();

    if (!one || !two) {
        return Value(0);
    }

    int year1 = getIntProperty(one, "_year");
    int month1 = getIntProperty(one, "_month");
    int day1 = getIntProperty(one, "_day");

    int year2 = getIntProperty(two, "_year");
    int month2 = getIntProperty(two, "_month");
    int day2 = getIntProperty(two, "_day");

    if (year1 != year2) return Value(year1 < year2 ? -1 : 1);
    if (month1 != month2) return Value(month1 < month2 ? -1 : 1);
    if (day1 != day2) return Value(day1 < day2 ? -1 : 1);
    return Value(0);
}

Value TemporalPlainDate::year(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();
    return obj->get_property("_year");
}

Value TemporalPlainDate::month(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();
    return obj->get_property("_month");
}

Value TemporalPlainDate::day(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();
    return obj->get_property("_day");
}

Value TemporalPlainDate::dayOfWeek(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");

    return Value(calcDayOfWeek(y, m, d) + 1);
}

Value TemporalPlainDate::dayOfYear(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");

    return Value(calcDayOfYear(y, m, d));
}

Value TemporalPlainDate::weekOfYear(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int doy = getIntProperty(obj, "_year");
    return Value((doy + 6) / 7);
}

Value TemporalPlainDate::monthCode(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int m = getIntProperty(obj, "_month");
    return Value("M" + padZero(m, 2));
}

Value TemporalPlainDate::daysInWeek(Context& ctx, const std::vector<Value>& args) {
    return Value(7);
}

Value TemporalPlainDate::daysInMonth(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");

    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && isLeapYear(y)) {
        return Value(29);
    }
    return Value(days[m - 1]);
}

Value TemporalPlainDate::daysInYear(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    return Value(isLeapYear(y) ? 366 : 365);
}

Value TemporalPlainDate::monthsInYear(Context& ctx, const std::vector<Value>& args) {
    return Value(12);
}

Value TemporalPlainDate::inLeapYear(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    return Value(isLeapYear(y));
}

Value TemporalPlainDate::toString(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");

    std::ostringstream oss;
    oss << padZero(y, 4) << "-" << padZero(m, 2) << "-" << padZero(d, 2);
    return Value(oss.str());
}

Value TemporalPlainDate::toJSON(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalPlainDate::toLocaleString(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalPlainDate::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainDate does not have a valueOf method")));
    return Value();
}

Value TemporalPlainDate::add(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* duration = args[1].as_object();
    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");

    y += getIntProperty(duration, "_years", 0);
    m += getIntProperty(duration, "_months", 0);
    d += getIntProperty(duration, "_days", 0);

    while (m > 12) {
        m -= 12;
        y++;
    }
    while (m < 1) {
        m += 12;
        y--;
    }

    return constructor(ctx, {Value(y), Value(m), Value(d)});
}

Value TemporalPlainDate::subtract(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* duration = args[1].as_object();
    int y = getIntProperty(obj, "_year");
    int m = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");

    y -= getIntProperty(duration, "_years", 0);
    m -= getIntProperty(duration, "_months", 0);
    d -= getIntProperty(duration, "_days", 0);

    while (m > 12) {
        m -= 12;
        y++;
    }
    while (m < 1) {
        m += 12;
        y--;
    }

    return constructor(ctx, {Value(y), Value(m), Value(d)});
}

Value TemporalPlainDate::with_(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* fields = args[1].as_object();
    int y = fields->has_own_property("year") ? getIntProperty(fields, "year") : getIntProperty(obj, "_year");
    int m = fields->has_own_property("month") ? getIntProperty(fields, "month") : getIntProperty(obj, "_month");
    int d = fields->has_own_property("day") ? getIntProperty(fields, "day") : getIntProperty(obj, "_day");

    return constructor(ctx, {Value(y), Value(m), Value(d)});
}

Value TemporalPlainDate::withCalendar(Context& ctx, const std::vector<Value>& args) {
    return Value(getThisObject(ctx, args, "Temporal.PlainDate"));
}

Value TemporalPlainDate::until(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return TemporalDuration::constructor(ctx, {});
    }

    Object* other = args[1].as_object();
    int y1 = getIntProperty(obj, "_year");
    int m1 = getIntProperty(obj, "_month");
    int d1 = getIntProperty(obj, "_day");

    int y2 = getIntProperty(other, "_year");
    int m2 = getIntProperty(other, "_month");
    int d2 = getIntProperty(other, "_day");

    Object* duration = new Object();
    duration->set_property("_years", Value(y2 - y1));
    duration->set_property("_months", Value(m2 - m1));
    duration->set_property("_days", Value(d2 - d1));
    duration->set_property("_class", Value(std::string("TemporalDuration")));
    return Value(duration);
}

Value TemporalPlainDate::since(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return TemporalDuration::constructor(ctx, {});
    }

    Object* other = args[1].as_object();
    int y1 = getIntProperty(obj, "_year");
    int m1 = getIntProperty(obj, "_month");
    int d1 = getIntProperty(obj, "_day");

    int y2 = getIntProperty(other, "_year");
    int m2 = getIntProperty(other, "_month");
    int d2 = getIntProperty(other, "_day");

    Object* duration = new Object();
    duration->set_property("_years", Value(y1 - y2));
    duration->set_property("_months", Value(m1 - m2));
    duration->set_property("_days", Value(d1 - d2));
    duration->set_property("_class", Value(std::string("TemporalDuration")));
    return Value(duration);
}

Value TemporalPlainDate::equals(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDate");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(false);
    }

    Object* other = args[1].as_object();
    return Value(
        getIntProperty(obj, "_year") == getIntProperty(other, "_year") &&
        getIntProperty(obj, "_month") == getIntProperty(other, "_month") &&
        getIntProperty(obj, "_day") == getIntProperty(other, "_day")
    );
}


TemporalPlainTime::TemporalPlainTime(int hour, int minute, int second, int millisecond, int microsecond, int nanosecond)
    : hour_(hour), minute_(minute), second_(second), millisecond_(millisecond),
      microsecond_(microsecond), nanosecond_(nanosecond) {}

Value TemporalPlainTime::constructor(Context& ctx, const std::vector<Value>& args) {
    int hour = args.size() > 0 ? static_cast<int>(args[0].to_number()) : 0;
    int minute = args.size() > 1 ? static_cast<int>(args[1].to_number()) : 0;
    int second = args.size() > 2 ? static_cast<int>(args[2].to_number()) : 0;
    int millisecond = args.size() > 3 ? static_cast<int>(args[3].to_number()) : 0;
    int microsecond = args.size() > 4 ? static_cast<int>(args[4].to_number()) : 0;
    int nanosecond = args.size() > 5 ? static_cast<int>(args[5].to_number()) : 0;

    Object* time = new Object();

    if (g_plainTime_prototype) {
        time->set_prototype(g_plainTime_prototype);
    }

    time->set_property("_hour", Value(hour));
    time->set_property("_minute", Value(minute));
    time->set_property("_second", Value(second));
    time->set_property("_millisecond", Value(millisecond));
    time->set_property("_microsecond", Value(microsecond));
    time->set_property("_nanosecond", Value(nanosecond));
    time->set_property("_class", Value(std::string("TemporalPlainTime")));
    return Value(time);
}

Value TemporalPlainTime::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainTime.from requires an argument")));
        return Value();
    }

    if (args[0].is_string()) {
        return constructor(ctx, {});
    }

    return args[0];
}

Value TemporalPlainTime::compare(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(0);
    }

    Object* one = args[0].as_object();
    Object* two = args[1].as_object();

    if (!one || !two) {
        return Value(0);
    }

    int h1 = getIntProperty(one, "_hour");
    int m1 = getIntProperty(one, "_minute");
    int s1 = getIntProperty(one, "_second");

    int h2 = getIntProperty(two, "_hour");
    int m2 = getIntProperty(two, "_minute");
    int s2 = getIntProperty(two, "_second");

    if (h1 != h2) return Value(h1 < h2 ? -1 : 1);
    if (m1 != m2) return Value(m1 < m2 ? -1 : 1);
    if (s1 != s2) return Value(s1 < s2 ? -1 : 1);
    return Value(0);
}

Value TemporalPlainTime::hour(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_hour");
}

Value TemporalPlainTime::minute(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_minute");
}

Value TemporalPlainTime::second(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_second");
}

Value TemporalPlainTime::millisecond(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_millisecond");
}

Value TemporalPlainTime::microsecond(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_microsecond");
}

Value TemporalPlainTime::nanosecond(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return obj->get_property("_nanosecond");
}

Value TemporalPlainTime::toString(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();

    int h = getIntProperty(obj, "_hour");
    int m = getIntProperty(obj, "_minute");
    int s = getIntProperty(obj, "_second");

    std::ostringstream oss;
    oss << padZero(h, 2) << ":" << padZero(m, 2) << ":" << padZero(s, 2);
    return Value(oss.str());
}

Value TemporalPlainTime::toJSON(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalPlainTime::toLocaleString(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value TemporalPlainTime::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainTime does not have a valueOf method")));
    return Value();
}

Value TemporalPlainTime::add(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* duration = args[1].as_object();
    int h = getIntProperty(obj, "_hour") + getIntProperty(duration, "_hours", 0);
    int m = getIntProperty(obj, "_minute") + getIntProperty(duration, "_minutes", 0);
    int s = getIntProperty(obj, "_second") + getIntProperty(duration, "_seconds", 0);

    m += s / 60;
    s %= 60;
    h += m / 60;
    m %= 60;
    h %= 24;

    return constructor(ctx, {Value(h), Value(m), Value(s)});
}

Value TemporalPlainTime::subtract(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* duration = args[1].as_object();
    int h = getIntProperty(obj, "_hour") - getIntProperty(duration, "_hours", 0);
    int m = getIntProperty(obj, "_minute") - getIntProperty(duration, "_minutes", 0);
    int s = getIntProperty(obj, "_second") - getIntProperty(duration, "_seconds", 0);

    while (s < 0) { s += 60; m--; }
    while (m < 0) { m += 60; h--; }
    while (h < 0) { h += 24; }

    return constructor(ctx, {Value(h), Value(m), Value(s)});
}

Value TemporalPlainTime::with_(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(obj);
    }

    Object* fields = args[1].as_object();
    int h = fields->has_own_property("hour") ? getIntProperty(fields, "hour") : getIntProperty(obj, "_hour");
    int m = fields->has_own_property("minute") ? getIntProperty(fields, "minute") : getIntProperty(obj, "_minute");
    int s = fields->has_own_property("second") ? getIntProperty(fields, "second") : getIntProperty(obj, "_second");

    return constructor(ctx, {Value(h), Value(m), Value(s)});
}

Value TemporalPlainTime::until(Context& ctx, const std::vector<Value>& args) {
    return TemporalDuration::constructor(ctx, {});
}

Value TemporalPlainTime::since(Context& ctx, const std::vector<Value>& args) {
    return TemporalDuration::constructor(ctx, {});
}

Value TemporalPlainTime::round(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();
    return Value(obj);
}

Value TemporalPlainTime::equals(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainTime");
    if (!obj) return Value();

    if (args.size() < 2 || !args[1].is_object()) {
        return Value(false);
    }

    Object* other = args[1].as_object();
    return Value(
        getIntProperty(obj, "_hour") == getIntProperty(other, "_hour") &&
        getIntProperty(obj, "_minute") == getIntProperty(other, "_minute") &&
        getIntProperty(obj, "_second") == getIntProperty(other, "_second")
    );
}


Value TemporalPlainDateTime::constructor(Context& ctx, const std::vector<Value>& args) {
    int year = args.size() > 0 ? static_cast<int>(args[0].to_number()) : 1970;
    int month = args.size() > 1 ? static_cast<int>(args[1].to_number()) : 1;
    int day = args.size() > 2 ? static_cast<int>(args[2].to_number()) : 1;
    int hour = args.size() > 3 ? static_cast<int>(args[3].to_number()) : 0;
    int minute = args.size() > 4 ? static_cast<int>(args[4].to_number()) : 0;
    int second = args.size() > 5 ? static_cast<int>(args[5].to_number()) : 0;

    Object* dt = new Object();

    if (g_plainDateTime_prototype) {
        dt->set_prototype(g_plainDateTime_prototype);
    }

    dt->set_property("_year", Value(year));
    dt->set_property("_month", Value(month));
    dt->set_property("_day", Value(day));
    dt->set_property("_hour", Value(hour));
    dt->set_property("_minute", Value(minute));
    dt->set_property("_second", Value(second));
    dt->set_property("_millisecond", Value(0));
    dt->set_property("_microsecond", Value(0));
    dt->set_property("_nanosecond", Value(0));
    dt->set_property("_calendar", Value(std::string("iso8601")));
    dt->set_property("_class", Value(std::string("TemporalPlainDateTime")));
    return Value(dt);
}

Value TemporalPlainDateTime::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

Value TemporalPlainDateTime::compare(Context& ctx, const std::vector<Value>& args) {
    return Value(0);
}

Value TemporalPlainDateTime::toString(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.PlainDateTime");
    if (!obj) return Value();

    int y = getIntProperty(obj, "_year");
    int mo = getIntProperty(obj, "_month");
    int d = getIntProperty(obj, "_day");
    int h = getIntProperty(obj, "_hour");
    int mi = getIntProperty(obj, "_minute");
    int s = getIntProperty(obj, "_second");

    std::ostringstream oss;
    oss << padZero(y, 4) << "-" << padZero(mo, 2) << "-" << padZero(d, 2) << "T"
        << padZero(h, 2) << ":" << padZero(mi, 2) << ":" << padZero(s, 2);
    return Value(oss.str());
}

#define TEMPORAL_STUB_METHOD(Class, Method) \
    Value Class::Method(Context& ctx, const std::vector<Value>& args) { \
        Object* obj = getThisObject(ctx, args, #Class); \
        if (!obj) return Value(); \
        return Value(obj); \
    }

#define TEMPORAL_GETTER_STUB(Class, Method, Property) \
    Value Class::Method(Context& ctx, const std::vector<Value>& args) { \
        Object* obj = getThisObject(ctx, args, #Class); \
        if (!obj) return Value(); \
        return obj->get_property(Property); \
    }

TEMPORAL_STUB_METHOD(TemporalPlainDateTime, add)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, subtract)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, with_)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, withPlainDate)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, withPlainTime)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, withCalendar)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, until)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, since)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, round)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, equals)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, toJSON)
TEMPORAL_STUB_METHOD(TemporalPlainDateTime, toLocaleString)

Value TemporalPlainDateTime::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.PlainDateTime does not have a valueOf method")));
    return Value();
}

TEMPORAL_GETTER_STUB(TemporalPlainDateTime, year, "_year")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, month, "_month")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, day, "_day")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, hour, "_hour")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, minute, "_minute")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, second, "_second")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, millisecond, "_millisecond")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, microsecond, "_microsecond")
TEMPORAL_GETTER_STUB(TemporalPlainDateTime, nanosecond, "_nanosecond")


TemporalDuration::TemporalDuration(double years, double months, double weeks, double days,
                                   double hours, double minutes, double seconds,
                                   double milliseconds, double microseconds, double nanoseconds)
    : years_(years), months_(months), weeks_(weeks), days_(days),
      hours_(hours), minutes_(minutes), seconds_(seconds),
      milliseconds_(milliseconds), microseconds_(microseconds), nanoseconds_(nanoseconds) {}

Value TemporalDuration::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* duration = new Object();

    if (g_duration_prototype) {
        duration->set_prototype(g_duration_prototype);
    }

    duration->set_property("_years", args.size() > 0 ? args[0] : Value(0));
    duration->set_property("_months", args.size() > 1 ? args[1] : Value(0));
    duration->set_property("_weeks", args.size() > 2 ? args[2] : Value(0));
    duration->set_property("_days", args.size() > 3 ? args[3] : Value(0));
    duration->set_property("_hours", args.size() > 4 ? args[4] : Value(0));
    duration->set_property("_minutes", args.size() > 5 ? args[5] : Value(0));
    duration->set_property("_seconds", args.size() > 6 ? args[6] : Value(0));
    duration->set_property("_milliseconds", args.size() > 7 ? args[7] : Value(0));
    duration->set_property("_microseconds", args.size() > 8 ? args[8] : Value(0));
    duration->set_property("_nanoseconds", args.size() > 9 ? args[9] : Value(0));
    duration->set_property("_class", Value(std::string("TemporalDuration")));
    return Value(duration);
}

Value TemporalDuration::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

Value TemporalDuration::compare(Context& ctx, const std::vector<Value>& args) {
    return Value(0);
}

TEMPORAL_GETTER_STUB(TemporalDuration, years, "_years")
TEMPORAL_GETTER_STUB(TemporalDuration, months, "_months")
TEMPORAL_GETTER_STUB(TemporalDuration, weeks, "_weeks")
TEMPORAL_GETTER_STUB(TemporalDuration, days, "_days")
TEMPORAL_GETTER_STUB(TemporalDuration, hours, "_hours")
TEMPORAL_GETTER_STUB(TemporalDuration, minutes, "_minutes")
TEMPORAL_GETTER_STUB(TemporalDuration, seconds, "_seconds")
TEMPORAL_GETTER_STUB(TemporalDuration, milliseconds, "_milliseconds")
TEMPORAL_GETTER_STUB(TemporalDuration, microseconds, "_microseconds")
TEMPORAL_GETTER_STUB(TemporalDuration, nanoseconds, "_nanoseconds")

Value TemporalDuration::sign(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Duration");
    if (!obj) return Value();

    double total = obj->get_property("_days").to_number() +
                   obj->get_property("_hours").to_number();

    if (total > 0) return Value(1);
    if (total < 0) return Value(-1);
    return Value(0);
}

Value TemporalDuration::blank(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Duration");
    if (!obj) return Value();

    double total = obj->get_property("_days").to_number() +
                   obj->get_property("_hours").to_number();

    return Value(total == 0);
}

Value TemporalDuration::toString(Context& ctx, const std::vector<Value>& args) {
    Object* obj = getThisObject(ctx, args, "Temporal.Duration");
    if (!obj) return Value();

    std::ostringstream oss;
    oss << "P";

    int days = getIntProperty(obj, "_days", 0);
    if (days != 0) oss << days << "D";

    int hours = getIntProperty(obj, "_hours", 0);
    int minutes = getIntProperty(obj, "_minutes", 0);
    int seconds = getIntProperty(obj, "_seconds", 0);

    if (hours != 0 || minutes != 0 || seconds != 0) {
        oss << "T";
        if (hours != 0) oss << hours << "H";
        if (minutes != 0) oss << minutes << "M";
        if (seconds != 0) oss << seconds << "S";
    }

    std::string result = oss.str();
    if (result == "P") result = "PT0S";

    return Value(result);
}

TEMPORAL_STUB_METHOD(TemporalDuration, with_)
TEMPORAL_STUB_METHOD(TemporalDuration, negated)
TEMPORAL_STUB_METHOD(TemporalDuration, abs)
TEMPORAL_STUB_METHOD(TemporalDuration, add)
TEMPORAL_STUB_METHOD(TemporalDuration, subtract)
TEMPORAL_STUB_METHOD(TemporalDuration, round)
TEMPORAL_STUB_METHOD(TemporalDuration, total)
TEMPORAL_STUB_METHOD(TemporalDuration, toJSON)
TEMPORAL_STUB_METHOD(TemporalDuration, toLocaleString)

Value TemporalDuration::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.Duration does not have a valueOf method")));
    return Value();
}

Value TemporalZonedDateTime::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* zdt = new Object();

    if (g_zonedDateTime_prototype) {
        zdt->set_prototype(g_zonedDateTime_prototype);
    }

    zdt->set_property("_nanoseconds", args.size() > 0 ? args[0] : Value(0));
    zdt->set_property("_timezone", args.size() > 1 ? args[1] : Value(std::string("UTC")));
    zdt->set_property("_calendar", Value(std::string("iso8601")));
    zdt->set_property("_class", Value(std::string("TemporalZonedDateTime")));
    return Value(zdt);
}

Value TemporalZonedDateTime::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

Value TemporalZonedDateTime::compare(Context& ctx, const std::vector<Value>& args) {
    return Value(0);
}

TEMPORAL_STUB_METHOD(TemporalZonedDateTime, add)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, subtract)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, with_)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, withCalendar)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, withTimeZone)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, until)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, since)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, round)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, equals)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, toString)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, toJSON)
TEMPORAL_STUB_METHOD(TemporalZonedDateTime, toLocaleString)

Value TemporalZonedDateTime::valueOf(Context& ctx, const std::vector<Value>& args) {
    ctx.throw_exception(Value(std::string("TypeError: Temporal.ZonedDateTime does not have a valueOf method")));
    return Value();
}

TEMPORAL_GETTER_STUB(TemporalZonedDateTime, epochSeconds, "_nanoseconds")
TEMPORAL_GETTER_STUB(TemporalZonedDateTime, epochMilliseconds, "_nanoseconds")
TEMPORAL_GETTER_STUB(TemporalZonedDateTime, epochNanoseconds, "_nanoseconds")

Value TemporalPlainYearMonth::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* ym = new Object();

    if (g_plainYearMonth_prototype) {
        ym->set_prototype(g_plainYearMonth_prototype);
    }

    ym->set_property("_year", args.size() > 0 ? args[0] : Value(1970));
    ym->set_property("_month", args.size() > 1 ? args[1] : Value(1));
    ym->set_property("_calendar", Value(std::string("iso8601")));
    ym->set_property("_class", Value(std::string("TemporalPlainYearMonth")));
    return Value(ym);
}

Value TemporalPlainYearMonth::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

Value TemporalPlainYearMonth::compare(Context& ctx, const std::vector<Value>& args) {
    return Value(0);
}

TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, add)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, subtract)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, with_)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, until)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, since)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, equals)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, toString)
TEMPORAL_STUB_METHOD(TemporalPlainYearMonth, toJSON)

TEMPORAL_GETTER_STUB(TemporalPlainYearMonth, year, "_year")
TEMPORAL_GETTER_STUB(TemporalPlainYearMonth, month, "_month")

Value TemporalPlainMonthDay::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* md = new Object();

    if (g_plainMonthDay_prototype) {
        md->set_prototype(g_plainMonthDay_prototype);
    }

    md->set_property("_month", args.size() > 0 ? args[0] : Value(1));
    md->set_property("_day", args.size() > 1 ? args[1] : Value(1));
    md->set_property("_calendar", Value(std::string("iso8601")));
    md->set_property("_class", Value(std::string("TemporalPlainMonthDay")));
    return Value(md);
}

Value TemporalPlainMonthDay::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

TEMPORAL_STUB_METHOD(TemporalPlainMonthDay, with_)
TEMPORAL_STUB_METHOD(TemporalPlainMonthDay, equals)
TEMPORAL_STUB_METHOD(TemporalPlainMonthDay, toString)
TEMPORAL_STUB_METHOD(TemporalPlainMonthDay, toJSON)

TEMPORAL_GETTER_STUB(TemporalPlainMonthDay, month, "_month")
TEMPORAL_GETTER_STUB(TemporalPlainMonthDay, day, "_day")

Value TemporalCalendar::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* cal = new Object();

    if (g_calendar_prototype) {
        cal->set_prototype(g_calendar_prototype);
    }

    cal->set_property("_id", args.size() > 0 ? args[0] : Value(std::string("iso8601")));
    cal->set_property("_class", Value(std::string("TemporalCalendar")));
    return Value(cal);
}

Value TemporalCalendar::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

TEMPORAL_STUB_METHOD(TemporalCalendar, toString)
TEMPORAL_STUB_METHOD(TemporalCalendar, toJSON)

Value TemporalTimeZone::constructor(Context& ctx, const std::vector<Value>& args) {
    Object* tz = new Object();

    if (g_timeZone_prototype) {
        tz->set_prototype(g_timeZone_prototype);
    }

    tz->set_property("_id", args.size() > 0 ? args[0] : Value(std::string("UTC")));
    tz->set_property("_class", Value(std::string("TemporalTimeZone")));
    return Value(tz);
}

Value TemporalTimeZone::from(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) return constructor(ctx, {});
    return args[0];
}

TEMPORAL_STUB_METHOD(TemporalTimeZone, toString)
TEMPORAL_STUB_METHOD(TemporalTimeZone, toJSON)


void Temporal::setup(Context& ctx) {
    auto temporal = ObjectFactory::create_object();

    auto now = ObjectFactory::create_object();
    auto now_instant = ObjectFactory::create_native_function("instant", TemporalNow::instant, 0);
    auto now_plainDateISO = ObjectFactory::create_native_function("plainDateISO", TemporalNow::plainDateISO, 0);
    auto now_plainTimeISO = ObjectFactory::create_native_function("plainTimeISO", TemporalNow::plainTimeISO, 0);
    auto now_plainDateTimeISO = ObjectFactory::create_native_function("plainDateTimeISO", TemporalNow::plainDateTimeISO, 0);
    auto now_zonedDateTimeISO = ObjectFactory::create_native_function("zonedDateTimeISO", TemporalNow::zonedDateTimeISO, 1);
    auto now_timeZoneId = ObjectFactory::create_native_function("timeZoneId", TemporalNow::timeZoneId, 0);

    now->set_property("instant", Value(now_instant.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    now->set_property("plainDateISO", Value(now_plainDateISO.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    now->set_property("plainTimeISO", Value(now_plainTimeISO.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    now->set_property("plainDateTimeISO", Value(now_plainDateTimeISO.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    now->set_property("zonedDateTimeISO", Value(now_zonedDateTimeISO.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    now->set_property("timeZoneId", Value(now_timeZoneId.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("Now", Value(now.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto instant_constructor = ObjectFactory::create_native_function("Instant", TemporalInstant::constructor, 1);
    auto instant_from = ObjectFactory::create_native_function("from", TemporalInstant::from, 1);
    auto instant_fromEpochMilliseconds = ObjectFactory::create_native_function("fromEpochMilliseconds", TemporalInstant::fromEpochMilliseconds, 1);
    auto instant_fromEpochNanoseconds = ObjectFactory::create_native_function("fromEpochNanoseconds", TemporalInstant::fromEpochNanoseconds, 1);
    auto instant_compare = ObjectFactory::create_native_function("compare", TemporalInstant::compare, 2);

    instant_constructor->set_property("from", Value(instant_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_constructor->set_property("fromEpochMilliseconds", Value(instant_fromEpochMilliseconds.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_constructor->set_property("fromEpochNanoseconds", Value(instant_fromEpochNanoseconds.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_constructor->set_property("compare", Value(instant_compare.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto instant_proto = ObjectFactory::create_object();
    auto inst_add = ObjectFactory::create_native_function("add", TemporalInstant::add, 1);
    auto inst_subtract = ObjectFactory::create_native_function("subtract", TemporalInstant::subtract, 1);
    auto inst_until = ObjectFactory::create_native_function("until", TemporalInstant::until, 1);
    auto inst_since = ObjectFactory::create_native_function("since", TemporalInstant::since, 1);
    auto inst_round = ObjectFactory::create_native_function("round", TemporalInstant::round, 1);
    auto inst_equals = ObjectFactory::create_native_function("equals", TemporalInstant::equals, 1);
    auto inst_toString = ObjectFactory::create_native_function("toString", TemporalInstant::toString, 0);
    auto inst_toJSON = ObjectFactory::create_native_function("toJSON", TemporalInstant::toJSON, 0);
    auto inst_toLocaleString = ObjectFactory::create_native_function("toLocaleString", TemporalInstant::toLocaleString, 0);
    auto inst_valueOf = ObjectFactory::create_native_function("valueOf", TemporalInstant::valueOf, 0);

    instant_proto->set_property("add", Value(inst_add.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("subtract", Value(inst_subtract.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("until", Value(inst_until.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("since", Value(inst_since.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("round", Value(inst_round.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("equals", Value(inst_equals.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("toString", Value(inst_toString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("toJSON", Value(inst_toJSON.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("toLocaleString", Value(inst_toLocaleString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    instant_proto->set_property("valueOf", Value(inst_valueOf.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    g_instant_prototype = instant_proto.get();

    instant_constructor->set_property("prototype", Value(instant_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("Instant", Value(instant_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainDate_constructor = ObjectFactory::create_native_function("PlainDate", TemporalPlainDate::constructor, 3);
    auto plainDate_from = ObjectFactory::create_native_function("from", TemporalPlainDate::from, 1);
    auto plainDate_compare = ObjectFactory::create_native_function("compare", TemporalPlainDate::compare, 2);

    plainDate_constructor->set_property("from", Value(plainDate_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_constructor->set_property("compare", Value(plainDate_compare.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainDate_proto = ObjectFactory::create_object();
    auto pd_add = ObjectFactory::create_native_function("add", TemporalPlainDate::add, 1);
    auto pd_subtract = ObjectFactory::create_native_function("subtract", TemporalPlainDate::subtract, 1);
    auto pd_with = ObjectFactory::create_native_function("with", TemporalPlainDate::with_, 1);
    auto pd_withCalendar = ObjectFactory::create_native_function("withCalendar", TemporalPlainDate::withCalendar, 1);
    auto pd_until = ObjectFactory::create_native_function("until", TemporalPlainDate::until, 1);
    auto pd_since = ObjectFactory::create_native_function("since", TemporalPlainDate::since, 1);
    auto pd_equals = ObjectFactory::create_native_function("equals", TemporalPlainDate::equals, 1);
    auto pd_toString = ObjectFactory::create_native_function("toString", TemporalPlainDate::toString, 0);
    auto pd_toJSON = ObjectFactory::create_native_function("toJSON", TemporalPlainDate::toJSON, 0);
    auto pd_toLocaleString = ObjectFactory::create_native_function("toLocaleString", TemporalPlainDate::toLocaleString, 0);
    auto pd_valueOf = ObjectFactory::create_native_function("valueOf", TemporalPlainDate::valueOf, 0);

    plainDate_proto->set_property("add", Value(pd_add.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("subtract", Value(pd_subtract.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("with", Value(pd_with.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("withCalendar", Value(pd_withCalendar.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("until", Value(pd_until.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("since", Value(pd_since.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("equals", Value(pd_equals.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("toString", Value(pd_toString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("toJSON", Value(pd_toJSON.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("toLocaleString", Value(pd_toLocaleString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDate_proto->set_property("valueOf", Value(pd_valueOf.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    g_plainDate_prototype = plainDate_proto.get();

    plainDate_constructor->set_property("prototype", Value(plainDate_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("PlainDate", Value(plainDate_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainTime_constructor = ObjectFactory::create_native_function("PlainTime", TemporalPlainTime::constructor, 6);
    auto plainTime_from = ObjectFactory::create_native_function("from", TemporalPlainTime::from, 1);
    auto plainTime_compare = ObjectFactory::create_native_function("compare", TemporalPlainTime::compare, 2);

    plainTime_constructor->set_property("from", Value(plainTime_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_constructor->set_property("compare", Value(plainTime_compare.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainTime_proto = ObjectFactory::create_object();
    auto pt_add = ObjectFactory::create_native_function("add", TemporalPlainTime::add, 1);
    auto pt_subtract = ObjectFactory::create_native_function("subtract", TemporalPlainTime::subtract, 1);
    auto pt_with = ObjectFactory::create_native_function("with", TemporalPlainTime::with_, 1);
    auto pt_until = ObjectFactory::create_native_function("until", TemporalPlainTime::until, 1);
    auto pt_since = ObjectFactory::create_native_function("since", TemporalPlainTime::since, 1);
    auto pt_round = ObjectFactory::create_native_function("round", TemporalPlainTime::round, 1);
    auto pt_equals = ObjectFactory::create_native_function("equals", TemporalPlainTime::equals, 1);
    auto pt_toString = ObjectFactory::create_native_function("toString", TemporalPlainTime::toString, 0);
    auto pt_toJSON = ObjectFactory::create_native_function("toJSON", TemporalPlainTime::toJSON, 0);
    auto pt_toLocaleString = ObjectFactory::create_native_function("toLocaleString", TemporalPlainTime::toLocaleString, 0);
    auto pt_valueOf = ObjectFactory::create_native_function("valueOf", TemporalPlainTime::valueOf, 0);

    plainTime_proto->set_property("add", Value(pt_add.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("subtract", Value(pt_subtract.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("with", Value(pt_with.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("until", Value(pt_until.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("since", Value(pt_since.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("round", Value(pt_round.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("equals", Value(pt_equals.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("toString", Value(pt_toString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("toJSON", Value(pt_toJSON.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("toLocaleString", Value(pt_toLocaleString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainTime_proto->set_property("valueOf", Value(pt_valueOf.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    g_plainTime_prototype = plainTime_proto.get();

    plainTime_constructor->set_property("prototype", Value(plainTime_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("PlainTime", Value(plainTime_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainDateTime_constructor = ObjectFactory::create_native_function("PlainDateTime", TemporalPlainDateTime::constructor, 6);
    auto plainDateTime_from = ObjectFactory::create_native_function("from", TemporalPlainDateTime::from, 1);
    auto plainDateTime_compare = ObjectFactory::create_native_function("compare", TemporalPlainDateTime::compare, 2);

    plainDateTime_constructor->set_property("from", Value(plainDateTime_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    plainDateTime_constructor->set_property("compare", Value(plainDateTime_compare.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainDateTime_proto = ObjectFactory::create_object();
    auto pdt_toString = ObjectFactory::create_native_function("toString", TemporalPlainDateTime::toString, 0);
    plainDateTime_proto->set_property("toString", Value(pdt_toString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    g_plainDateTime_prototype = plainDateTime_proto.get();

    plainDateTime_constructor->set_property("prototype", Value(plainDateTime_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("PlainDateTime", Value(plainDateTime_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto duration_constructor = ObjectFactory::create_native_function("Duration", TemporalDuration::constructor, 10);
    auto duration_from = ObjectFactory::create_native_function("from", TemporalDuration::from, 1);
    auto duration_compare = ObjectFactory::create_native_function("compare", TemporalDuration::compare, 2);

    duration_constructor->set_property("from", Value(duration_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    duration_constructor->set_property("compare", Value(duration_compare.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto duration_proto = ObjectFactory::create_object();
    auto dur_toString = ObjectFactory::create_native_function("toString", TemporalDuration::toString, 0);
    duration_proto->set_property("toString", Value(dur_toString.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    g_duration_prototype = duration_proto.get();

    duration_constructor->set_property("prototype", Value(duration_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::None));
    temporal->set_property("Duration", Value(duration_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto zonedDateTime_constructor = ObjectFactory::create_native_function("ZonedDateTime", TemporalZonedDateTime::constructor, 2);
    auto zonedDateTime_from = ObjectFactory::create_native_function("from", TemporalZonedDateTime::from, 1);

    zonedDateTime_constructor->set_property("from", Value(zonedDateTime_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto zonedDateTime_proto = ObjectFactory::create_object();

    g_zonedDateTime_prototype = zonedDateTime_proto.get();

    zonedDateTime_constructor->set_property("prototype", Value(zonedDateTime_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("ZonedDateTime", Value(zonedDateTime_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainYearMonth_constructor = ObjectFactory::create_native_function("PlainYearMonth", TemporalPlainYearMonth::constructor, 2);
    auto plainYearMonth_from = ObjectFactory::create_native_function("from", TemporalPlainYearMonth::from, 1);

    plainYearMonth_constructor->set_property("from", Value(plainYearMonth_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainYearMonth_proto = ObjectFactory::create_object();

    g_plainYearMonth_prototype = plainYearMonth_proto.get();

    plainYearMonth_constructor->set_property("prototype", Value(plainYearMonth_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("PlainYearMonth", Value(plainYearMonth_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainMonthDay_constructor = ObjectFactory::create_native_function("PlainMonthDay", TemporalPlainMonthDay::constructor, 2);
    auto plainMonthDay_from = ObjectFactory::create_native_function("from", TemporalPlainMonthDay::from, 1);

    plainMonthDay_constructor->set_property("from", Value(plainMonthDay_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto plainMonthDay_proto = ObjectFactory::create_object();

    g_plainMonthDay_prototype = plainMonthDay_proto.get();

    plainMonthDay_constructor->set_property("prototype", Value(plainMonthDay_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("PlainMonthDay", Value(plainMonthDay_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto calendar_constructor = ObjectFactory::create_native_function("Calendar", TemporalCalendar::constructor, 1);
    auto calendar_from = ObjectFactory::create_native_function("from", TemporalCalendar::from, 1);

    calendar_constructor->set_property("from", Value(calendar_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto calendar_proto = ObjectFactory::create_object();

    g_calendar_prototype = calendar_proto.get();

    calendar_constructor->set_property("prototype", Value(calendar_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("Calendar", Value(calendar_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto timeZone_constructor = ObjectFactory::create_native_function("TimeZone", TemporalTimeZone::constructor, 1);
    auto timeZone_from = ObjectFactory::create_native_function("from", TemporalTimeZone::from, 1);

    timeZone_constructor->set_property("from", Value(timeZone_from.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto timeZone_proto = ObjectFactory::create_object();

    g_timeZone_prototype = timeZone_proto.get();

    timeZone_constructor->set_property("prototype", Value(timeZone_proto.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    temporal->set_property("TimeZone", Value(timeZone_constructor.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    ctx.register_built_in_object("Temporal", temporal.release());
}

}
