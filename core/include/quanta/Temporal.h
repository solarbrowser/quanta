/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TEMPORAL_H
#define QUANTA_TEMPORAL_H

#include "quanta/Value.h"
#include <chrono>
#include <ctime>
#include <string>
#include <cmath>
#include <vector>

namespace Quanta {

class Context;

/**
 * JavaScript Temporal API implementation (TC39 Stage 3)
 * Modern date/time API with better design than Date
 */

class TemporalInstant;
class TemporalPlainDate;
class TemporalPlainTime;
class TemporalPlainDateTime;
class TemporalZonedDateTime;
class TemporalDuration;
class TemporalCalendar;
class TemporalTimeZone;

/**
 * Temporal.Now namespace - current date/time accessors
 */
class TemporalNow {
public:
    static Value instant(Context& ctx, const std::vector<Value>& args);
    static Value plainDateISO(Context& ctx, const std::vector<Value>& args);
    static Value plainTimeISO(Context& ctx, const std::vector<Value>& args);
    static Value plainDateTimeISO(Context& ctx, const std::vector<Value>& args);
    static Value zonedDateTimeISO(Context& ctx, const std::vector<Value>& args);
    static Value timeZoneId(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.Instant - Represents an exact moment in time
 */
class TemporalInstant {
private:
    int64_t nanoseconds_;

public:
    TemporalInstant(int64_t nanoseconds);

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value fromEpochMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value fromEpochNanoseconds(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value epochSeconds(Context& ctx, const std::vector<Value>& args);
    static Value epochMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value epochMicroseconds(Context& ctx, const std::vector<Value>& args);
    static Value epochNanoseconds(Context& ctx, const std::vector<Value>& args);

    int64_t getNanoseconds() const { return nanoseconds_; }
};

/**
 * Temporal.PlainDate - Represents a calendar date (no time)
 */
class TemporalPlainDate {
private:
    int year_;
    int month_;
    int day_;
    std::string calendar_;

public:
    TemporalPlainDate(int year, int month, int day, const std::string& calendar = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value withCalendar(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value year(Context& ctx, const std::vector<Value>& args);
    static Value month(Context& ctx, const std::vector<Value>& args);
    static Value day(Context& ctx, const std::vector<Value>& args);
    static Value dayOfWeek(Context& ctx, const std::vector<Value>& args);
    static Value dayOfYear(Context& ctx, const std::vector<Value>& args);
    static Value weekOfYear(Context& ctx, const std::vector<Value>& args);
    static Value monthCode(Context& ctx, const std::vector<Value>& args);
    static Value daysInWeek(Context& ctx, const std::vector<Value>& args);
    static Value daysInMonth(Context& ctx, const std::vector<Value>& args);
    static Value daysInYear(Context& ctx, const std::vector<Value>& args);
    static Value monthsInYear(Context& ctx, const std::vector<Value>& args);
    static Value inLeapYear(Context& ctx, const std::vector<Value>& args);

    int getYear() const { return year_; }
    int getMonth() const { return month_; }
    int getDay() const { return day_; }
};

/**
 * Temporal.PlainTime - Represents a wall-clock time (no date)
 */
class TemporalPlainTime {
private:
    int hour_;
    int minute_;
    int second_;
    int millisecond_;
    int microsecond_;
    int nanosecond_;

public:
    TemporalPlainTime(int hour, int minute, int second, int millisecond = 0, int microsecond = 0, int nanosecond = 0);

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value hour(Context& ctx, const std::vector<Value>& args);
    static Value minute(Context& ctx, const std::vector<Value>& args);
    static Value second(Context& ctx, const std::vector<Value>& args);
    static Value millisecond(Context& ctx, const std::vector<Value>& args);
    static Value microsecond(Context& ctx, const std::vector<Value>& args);
    static Value nanosecond(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.PlainDateTime - Represents a date and time (no timezone)
 */
class TemporalPlainDateTime {
private:
    int year_;
    int month_;
    int day_;
    int hour_;
    int minute_;
    int second_;
    int millisecond_;
    int microsecond_;
    int nanosecond_;
    std::string calendar_;

public:
    TemporalPlainDateTime(int year, int month, int day, int hour, int minute, int second,
                          int millisecond = 0, int microsecond = 0, int nanosecond = 0,
                          const std::string& calendar = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value withPlainDate(Context& ctx, const std::vector<Value>& args);
    static Value withPlainTime(Context& ctx, const std::vector<Value>& args);
    static Value withCalendar(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value year(Context& ctx, const std::vector<Value>& args);
    static Value month(Context& ctx, const std::vector<Value>& args);
    static Value day(Context& ctx, const std::vector<Value>& args);

    static Value hour(Context& ctx, const std::vector<Value>& args);
    static Value minute(Context& ctx, const std::vector<Value>& args);
    static Value second(Context& ctx, const std::vector<Value>& args);
    static Value millisecond(Context& ctx, const std::vector<Value>& args);
    static Value microsecond(Context& ctx, const std::vector<Value>& args);
    static Value nanosecond(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.Duration - Represents a duration of time
 */
class TemporalDuration {
private:
    double years_;
    double months_;
    double weeks_;
    double days_;
    double hours_;
    double minutes_;
    double seconds_;
    double milliseconds_;
    double microseconds_;
    double nanoseconds_;

public:
    TemporalDuration(double years = 0, double months = 0, double weeks = 0, double days = 0,
                     double hours = 0, double minutes = 0, double seconds = 0,
                     double milliseconds = 0, double microseconds = 0, double nanoseconds = 0);

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value negated(Context& ctx, const std::vector<Value>& args);
    static Value abs(Context& ctx, const std::vector<Value>& args);
    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value total(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value years(Context& ctx, const std::vector<Value>& args);
    static Value months(Context& ctx, const std::vector<Value>& args);
    static Value weeks(Context& ctx, const std::vector<Value>& args);
    static Value days(Context& ctx, const std::vector<Value>& args);
    static Value hours(Context& ctx, const std::vector<Value>& args);
    static Value minutes(Context& ctx, const std::vector<Value>& args);
    static Value seconds(Context& ctx, const std::vector<Value>& args);
    static Value milliseconds(Context& ctx, const std::vector<Value>& args);
    static Value microseconds(Context& ctx, const std::vector<Value>& args);
    static Value nanoseconds(Context& ctx, const std::vector<Value>& args);
    static Value sign(Context& ctx, const std::vector<Value>& args);
    static Value blank(Context& ctx, const std::vector<Value>& args);

    double getYears() const { return years_; }
    double getMonths() const { return months_; }
    double getWeeks() const { return weeks_; }
    double getDays() const { return days_; }
    double getHours() const { return hours_; }
    double getMinutes() const { return minutes_; }
    double getSeconds() const { return seconds_; }
    double getMilliseconds() const { return milliseconds_; }
    double getMicroseconds() const { return microseconds_; }
    double getNanoseconds() const { return nanoseconds_; }
};

/**
 * Temporal.ZonedDateTime - Date/time with timezone
 */
class TemporalZonedDateTime {
private:
    int64_t nanoseconds_;
    std::string timezone_;
    std::string calendar_;

public:
    TemporalZonedDateTime(int64_t nanoseconds, const std::string& timezone, const std::string& calendar = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value withCalendar(Context& ctx, const std::vector<Value>& args);
    static Value withTimeZone(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
    static Value toLocaleString(Context& ctx, const std::vector<Value>& args);
    static Value valueOf(Context& ctx, const std::vector<Value>& args);

    static Value epochSeconds(Context& ctx, const std::vector<Value>& args);
    static Value epochMilliseconds(Context& ctx, const std::vector<Value>& args);
    static Value epochNanoseconds(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.PlainYearMonth - Represents a year and month
 */
class TemporalPlainYearMonth {
private:
    int year_;
    int month_;
    std::string calendar_;

public:
    TemporalPlainYearMonth(int year, int month, const std::string& calendar = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);
    static Value compare(Context& ctx, const std::vector<Value>& args);

    static Value add(Context& ctx, const std::vector<Value>& args);
    static Value subtract(Context& ctx, const std::vector<Value>& args);
    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value until(Context& ctx, const std::vector<Value>& args);
    static Value since(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);

    static Value year(Context& ctx, const std::vector<Value>& args);
    static Value month(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.PlainMonthDay - Represents a month and day
 */
class TemporalPlainMonthDay {
private:
    int month_;
    int day_;
    std::string calendar_;

public:
    TemporalPlainMonthDay(int month, int day, const std::string& calendar = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);

    static Value with_(Context& ctx, const std::vector<Value>& args);
    static Value equals(Context& ctx, const std::vector<Value>& args);
    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);

    static Value month(Context& ctx, const std::vector<Value>& args);
    static Value day(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.Calendar - Calendar system
 */
class TemporalCalendar {
private:
    std::string id_;

public:
    TemporalCalendar(const std::string& id = "iso8601");

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);

    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
};

/**
 * Temporal.TimeZone - Time zone
 */
class TemporalTimeZone {
private:
    std::string id_;

public:
    TemporalTimeZone(const std::string& id);

    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value from(Context& ctx, const std::vector<Value>& args);

    static Value toString(Context& ctx, const std::vector<Value>& args);
    static Value toJSON(Context& ctx, const std::vector<Value>& args);
};

/**
 * Main Temporal namespace setup
 */
namespace Temporal {
    void setup(Context& ctx);
}

}

#endif
