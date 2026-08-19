/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TEMPORAL_H
#define QUANTA_TEMPORAL_H

#include "quanta/core/runtime/Value.h"
#include <span>
#include <chrono>
#include <ctime>
#include <string>
#include <cmath>
#include <vector>

namespace Quanta {

class Context;

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
    static Value instant(Context& ctx, std::span<const Value> args, Value receiver);
    static Value plainDateISO(Context& ctx, std::span<const Value> args, Value receiver);
    static Value plainTimeISO(Context& ctx, std::span<const Value> args, Value receiver);
    static Value plainDateTimeISO(Context& ctx, std::span<const Value> args, Value receiver);
    static Value zonedDateTimeISO(Context& ctx, std::span<const Value> args, Value receiver);
    static Value timeZoneId(Context& ctx, std::span<const Value> args, Value receiver);
};

/**
 * Temporal.Instant - Represents an exact moment in time
 */
class TemporalInstant {
private:
    int64_t nanoseconds_;

public:
    TemporalInstant(int64_t nanoseconds);

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value fromEpochMilliseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value fromEpochNanoseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value epochSeconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value epochMilliseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value epochMicroseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value epochNanoseconds(Context& ctx, std::span<const Value> args, Value receiver);

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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withCalendar(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value year(Context& ctx, std::span<const Value> args, Value receiver);
    static Value month(Context& ctx, std::span<const Value> args, Value receiver);
    static Value day(Context& ctx, std::span<const Value> args, Value receiver);
    static Value dayOfWeek(Context& ctx, std::span<const Value> args, Value receiver);
    static Value dayOfYear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weekOfYear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value monthCode(Context& ctx, std::span<const Value> args, Value receiver);
    static Value daysInWeek(Context& ctx, std::span<const Value> args, Value receiver);
    static Value daysInMonth(Context& ctx, std::span<const Value> args, Value receiver);
    static Value daysInYear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value monthsInYear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value inLeapYear(Context& ctx, std::span<const Value> args, Value receiver);

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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value hour(Context& ctx, std::span<const Value> args, Value receiver);
    static Value minute(Context& ctx, std::span<const Value> args, Value receiver);
    static Value second(Context& ctx, std::span<const Value> args, Value receiver);
    static Value millisecond(Context& ctx, std::span<const Value> args, Value receiver);
    static Value microsecond(Context& ctx, std::span<const Value> args, Value receiver);
    static Value nanosecond(Context& ctx, std::span<const Value> args, Value receiver);
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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withPlainDate(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withPlainTime(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withCalendar(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value year(Context& ctx, std::span<const Value> args, Value receiver);
    static Value month(Context& ctx, std::span<const Value> args, Value receiver);
    static Value day(Context& ctx, std::span<const Value> args, Value receiver);

    static Value hour(Context& ctx, std::span<const Value> args, Value receiver);
    static Value minute(Context& ctx, std::span<const Value> args, Value receiver);
    static Value second(Context& ctx, std::span<const Value> args, Value receiver);
    static Value millisecond(Context& ctx, std::span<const Value> args, Value receiver);
    static Value microsecond(Context& ctx, std::span<const Value> args, Value receiver);
    static Value nanosecond(Context& ctx, std::span<const Value> args, Value receiver);
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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value negated(Context& ctx, std::span<const Value> args, Value receiver);
    static Value abs(Context& ctx, std::span<const Value> args, Value receiver);
    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value total(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value years(Context& ctx, std::span<const Value> args, Value receiver);
    static Value months(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weeks(Context& ctx, std::span<const Value> args, Value receiver);
    static Value days(Context& ctx, std::span<const Value> args, Value receiver);
    static Value hours(Context& ctx, std::span<const Value> args, Value receiver);
    static Value minutes(Context& ctx, std::span<const Value> args, Value receiver);
    static Value seconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value milliseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value microseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value nanoseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value sign(Context& ctx, std::span<const Value> args, Value receiver);
    static Value blank(Context& ctx, std::span<const Value> args, Value receiver);

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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withCalendar(Context& ctx, std::span<const Value> args, Value receiver);
    static Value withTimeZone(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toLocaleString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value valueOf(Context& ctx, std::span<const Value> args, Value receiver);

    static Value epochSeconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value epochMilliseconds(Context& ctx, std::span<const Value> args, Value receiver);
    static Value epochNanoseconds(Context& ctx, std::span<const Value> args, Value receiver);
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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);
    static Value compare(Context& ctx, std::span<const Value> args, Value receiver);

    static Value add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value subtract(Context& ctx, std::span<const Value> args, Value receiver);
    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value until(Context& ctx, std::span<const Value> args, Value receiver);
    static Value since(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);

    static Value year(Context& ctx, std::span<const Value> args, Value receiver);
    static Value month(Context& ctx, std::span<const Value> args, Value receiver);
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

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);

    static Value with_(Context& ctx, std::span<const Value> args, Value receiver);
    static Value equals(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);

    static Value month(Context& ctx, std::span<const Value> args, Value receiver);
    static Value day(Context& ctx, std::span<const Value> args, Value receiver);
};

/**
 * Temporal.Calendar - Calendar system
 */
class TemporalCalendar {
private:
    std::string id_;

public:
    TemporalCalendar(const std::string& id = "iso8601");

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);

    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
};

/**
 * Temporal.TimeZone - Time zone
 */
class TemporalTimeZone {
private:
    std::string id_;

public:
    TemporalTimeZone(const std::string& id);

    static Value constructor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value from(Context& ctx, std::span<const Value> args, Value receiver);

    static Value toString(Context& ctx, std::span<const Value> args, Value receiver);
    static Value toJSON(Context& ctx, std::span<const Value> args, Value receiver);
};

/**
 * Main Temporal namespace setup
 */
namespace Temporal {
    void setup(Context& ctx);
}

}

#endif
