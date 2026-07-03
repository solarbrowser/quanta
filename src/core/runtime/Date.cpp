/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Date.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace Quanta {

namespace {

constexpr double kMsPerSecond = 1000.0;
constexpr double kMsPerMinute = 60000.0;
constexpr double kMsPerHour = 3600000.0;
constexpr double kMsPerDay = 86400000.0;
constexpr double kMaxTimeValue = 8.64e15;

const char* const kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const int kMonthStarts[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

double nan_value() { return std::numeric_limits<double>::quiet_NaN(); }

double js_modulo(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    return r;
}

double to_integer(double d) {
    if (std::isnan(d)) return 0.0;
    return std::trunc(d);
}

double day_number(double t) { return std::floor(t / kMsPerDay); }
double time_within_day(double t) { return js_modulo(t, kMsPerDay); }

bool is_leap_year(double y) {
    if (js_modulo(y, 4.0) != 0.0) return false;
    if (js_modulo(y, 100.0) != 0.0) return true;
    return js_modulo(y, 400.0) == 0.0;
}

double day_from_year(double y) {
    return 365.0 * (y - 1970.0) + std::floor((y - 1969.0) / 4.0) -
           std::floor((y - 1901.0) / 100.0) + std::floor((y - 1601.0) / 400.0);
}

double time_from_year(double y) { return kMsPerDay * day_from_year(y); }

double year_from_time(double t) {
    double y = std::floor(t / (kMsPerDay * 365.2425)) + 1970.0;
    if (time_from_year(y) > t) {
        while (time_from_year(y) > t) y -= 1.0;
    } else {
        while (time_from_year(y + 1.0) <= t) y += 1.0;
    }
    return y;
}

double day_within_year(double t) { return day_number(t) - day_from_year(year_from_time(t)); }

int month_from_time(double t) {
    double d = day_within_year(t);
    int leap = is_leap_year(year_from_time(t)) ? 1 : 0;
    for (int m = 11; m >= 1; --m) {
        if (d >= kMonthStarts[m] + (m >= 2 ? leap : 0)) return m;
    }
    return 0;
}

double date_from_time(double t) {
    double d = day_within_year(t);
    int m = month_from_time(t);
    int leap = is_leap_year(year_from_time(t)) ? 1 : 0;
    return d - (kMonthStarts[m] + (m >= 2 ? leap : 0)) + 1.0;
}

double week_day(double t) { return js_modulo(day_number(t) + 4.0, 7.0); }
double hour_from_time(double t) { return js_modulo(std::floor(t / kMsPerHour), 24.0); }
double min_from_time(double t) { return js_modulo(std::floor(t / kMsPerMinute), 60.0); }
double sec_from_time(double t) { return js_modulo(std::floor(t / kMsPerSecond), 60.0); }
double ms_from_time(double t) { return js_modulo(t, kMsPerSecond); }

// The volatile temporaries force the spec's individually rounded IEEE operations;
// otherwise FMA contraction merges multiply+add into a single rounding.
double make_time(double hour, double min, double sec, double ms) {
    if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) || !std::isfinite(ms)) {
        return nan_value();
    }
    double h = to_integer(hour), m = to_integer(min), s = to_integer(sec), milli = to_integer(ms);
    volatile double hms_h = h * kMsPerHour;
    volatile double hms_m = m * kMsPerMinute;
    volatile double hms_s = s * kMsPerSecond;
    volatile double hm = hms_h + hms_m;
    volatile double hms = hm + hms_s;
    return hms + milli;
}

double make_day(double year, double month, double date) {
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) {
        return nan_value();
    }
    double y = to_integer(year), m = to_integer(month), dt = to_integer(date);
    double ym = y + std::floor(m / 12.0);
    // No time value exists for years outside the representable range.
    if (std::isnan(ym) || ym < -271821.0 || ym > 275760.0) return nan_value();
    int mn = static_cast<int>(js_modulo(m, 12.0));
    double d = day_from_year(ym) + kMonthStarts[mn] + ((mn >= 2 && is_leap_year(ym)) ? 1.0 : 0.0);
    return d + dt - 1.0;
}

double make_date(double day, double time) {
    if (!std::isfinite(day) || !std::isfinite(time)) return nan_value();
    volatile double product = day * kMsPerDay;
    return product + time;
}

double time_clip(double t) {
    if (!std::isfinite(t) || std::fabs(t) > kMaxTimeValue) return nan_value();
    double r = to_integer(t);
    return r == 0.0 ? 0.0 : r;
}

bool local_tm_at(double utc_ms, std::tm& out) {
    double q = utc_ms;
    if (q > kMaxTimeValue) q = kMaxTimeValue;
    if (q < -kMaxTimeValue) q = -kMaxTimeValue;
    std::time_t tt = static_cast<std::time_t>(std::floor(q / 1000.0));
#ifdef _WIN32
    return localtime_s(&out, &tt) == 0;
#else
    return localtime_r(&tt, &out) != nullptr;
#endif
}

double tz_offset_ms(double utc_ms) {
    if (std::isnan(utc_ms)) return 0.0;
    std::tm tmv{};
    if (!local_tm_at(utc_ms, tmv)) return 0.0;
    double q = utc_ms;
    if (q > kMaxTimeValue) q = kMaxTimeValue;
    if (q < -kMaxTimeValue) q = -kMaxTimeValue;
    double d = make_day(tmv.tm_year + 1900.0, tmv.tm_mon, tmv.tm_mday);
    double local = make_date(d, make_time(tmv.tm_hour, tmv.tm_min, tmv.tm_sec, 0.0));
    return local - std::floor(q / 1000.0) * 1000.0;
}

double local_time(double t) { return t + tz_offset_ms(t); }

double utc_time(double local) {
    double off = tz_offset_ms(local);
    off = tz_offset_ms(local - off);
    return local - off;
}

double get_date_value(Object* obj) {
    return obj->get_property("_timestamp").to_number();
}

void set_date_value(Object* obj, double t) {
    obj->set_property("_timestamp", Value(t));
}

Object* this_date_object(Context& ctx) {
    Object* obj = ctx.get_this_binding();
    if (!obj || obj->get_type() != Object::ObjectType::Date ||
        ctx.original_this_was_primitive() || ctx.original_this_was_nullish()) {
        ctx.throw_type_error("this is not a Date object");
        return nullptr;
    }
    return obj;
}

bool this_time_value(Context& ctx, double& t) {
    Object* obj = this_date_object(ctx);
    if (!obj) return false;
    t = get_date_value(obj);
    return true;
}

Value nan_result() { return Value(nan_value()); }

Value ordinary_to_primitive(Context& ctx, Object* obj, bool prefer_string) {
    const char* order[2] = {"valueOf", "toString"};
    if (prefer_string) { order[0] = "toString"; order[1] = "valueOf"; }
    for (const char* name : order) {
        Value fn = obj->get_property(name);
        if (ctx.has_exception()) return Value();
        if (fn.is_function()) {
            Value r = fn.as_function()->call(ctx, {}, Value(obj));
            if (ctx.has_exception()) return Value();
            if (!r.is_object() && !r.is_function()) return r;
        }
    }
    ctx.throw_type_error("Cannot convert object to primitive value");
    return Value();
}

Value to_primitive(Context& ctx, const Value& v, const char* hint) {
    if (!v.is_object() && !v.is_function()) return v;
    Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
    Symbol* sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (sym) {
        Value fn = obj->get_property(sym->to_property_key());
        if (ctx.has_exception()) return Value();
        if (!fn.is_undefined() && !fn.is_null()) {
            if (!fn.is_function()) {
                ctx.throw_type_error("Symbol.toPrimitive is not a function");
                return Value();
            }
            Value r = fn.as_function()->call(ctx, {Value(std::string(hint))}, v);
            if (ctx.has_exception()) return Value();
            if (r.is_object() || r.is_function()) {
                ctx.throw_type_error("Symbol.toPrimitive returned an object");
                return Value();
            }
            return r;
        }
    }
    return ordinary_to_primitive(ctx, obj, std::strcmp(hint, "string") == 0);
}

std::string padded_year(double y) {
    char buf[16];
    long long abs_y = static_cast<long long>(std::fabs(y));
    std::snprintf(buf, sizeof buf, "%s%04lld", y < 0 ? "-" : "", abs_y);
    return buf;
}

std::string date_string(double t) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %s %02d ",
                  kDayNames[static_cast<int>(week_day(t))],
                  kMonthNames[month_from_time(t)],
                  static_cast<int>(date_from_time(t)));
    return buf + padded_year(year_from_time(t));
}

std::string time_string(double t) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d GMT",
                  static_cast<int>(hour_from_time(t)),
                  static_cast<int>(min_from_time(t)),
                  static_cast<int>(sec_from_time(t)));
    return buf;
}

std::string time_zone_string(double tv) {
    double off = tz_offset_ms(tv);
    char sign = off >= 0 ? '+' : '-';
    int mins = static_cast<int>(std::fabs(off) / kMsPerMinute);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%02d%02d", sign, mins / 60, mins % 60);
    std::string s = buf;
#ifndef _WIN32
    std::tm tmv{};
    if (local_tm_at(tv, tmv) && tmv.tm_zone && *tmv.tm_zone) {
        s += std::string(" (") + tmv.tm_zone + ")";
    }
#endif
    return s;
}

// Fields ordered as in MakeDay/MakeTime; setters write a contiguous run of them.
enum DateField { kFieldYear = 0, kFieldMonth, kFieldDate, kFieldHour, kFieldMin, kFieldSec, kFieldMs };

Value set_date_fields(Context& ctx, const std::vector<Value>& args, bool utc, int first, int count) {
    Object* obj = this_date_object(ctx);
    if (!obj) return Value();
    double t = get_date_value(obj);

    double coerced[4];
    bool provided[4] = {false, false, false, false};
    for (int k = 0; k < count; ++k) {
        if (k == 0 || static_cast<size_t>(k) < args.size()) {
            Value arg = static_cast<size_t>(k) < args.size() ? args[k] : Value();
            coerced[k] = arg.to_number();
            if (ctx.has_exception()) return Value();
            provided[k] = true;
        }
    }

    if (first == kFieldYear) {
        t = std::isnan(t) ? 0.0 : (utc ? t : local_time(t));
    } else {
        if (std::isnan(t)) return nan_result();
        if (!utc) t = local_time(t);
    }

    double f[7] = {year_from_time(t), static_cast<double>(month_from_time(t)), date_from_time(t),
                   hour_from_time(t), min_from_time(t), sec_from_time(t), ms_from_time(t)};
    for (int k = 0; k < count; ++k) {
        if (provided[k]) f[first + k] = coerced[k];
    }

    double new_date = make_date(make_day(f[kFieldYear], f[kFieldMonth], f[kFieldDate]),
                                make_time(f[kFieldHour], f[kFieldMin], f[kFieldSec], f[kFieldMs]));
    double u = time_clip(utc ? new_date : utc_time(new_date));
    set_date_value(obj, u);
    return Value(u);
}

bool parse_fixed_digits(const std::string& s, size_t& i, int count, long long& out) {
    long long v = 0;
    for (int k = 0; k < count; ++k) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    out = v;
    return true;
}

// ECMA-262 Date Time String Format. Sets ok=false when the string is not ISO-shaped.
double parse_iso_string(const std::string& s, bool& ok) {
    ok = false;
    size_t i = 0;
    double year;
    long long v;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        char sign = s[i++];
        if (!parse_fixed_digits(s, i, 6, v)) return nan_value();
        if (sign == '-' && v == 0) return nan_value();
        year = sign == '-' ? -static_cast<double>(v) : static_cast<double>(v);
    } else {
        if (!parse_fixed_digits(s, i, 4, v)) return nan_value();
        year = static_cast<double>(v);
    }
    ok = true;

    long long month = 1, day = 1;
    if (i < s.size() && s[i] == '-') {
        ++i;
        if (!parse_fixed_digits(s, i, 2, month) || month < 1 || month > 12) return nan_value();
        if (i < s.size() && s[i] == '-') {
            ++i;
            if (!parse_fixed_digits(s, i, 2, day) || day < 1 || day > 31) return nan_value();
        }
    }

    bool date_only = true;
    long long hour = 0, minute = 0, second = 0, ms = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        ++i;
        date_only = false;
        if (!parse_fixed_digits(s, i, 2, hour) || hour > 24) return nan_value();
        if (i >= s.size() || s[i] != ':') return nan_value();
        ++i;
        if (!parse_fixed_digits(s, i, 2, minute) || minute > 59) return nan_value();
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!parse_fixed_digits(s, i, 2, second) || second > 59) return nan_value();
            if (i < s.size() && s[i] == '.') {
                ++i;
                int n = 0;
                long long frac = 0;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    if (n < 3) frac = frac * 10 + (s[i] - '0');
                    ++n;
                    ++i;
                }
                if (n == 0) return nan_value();
                while (n < 3) { frac *= 10; ++n; }
                ms = frac;
            }
        }
        if (hour == 24 && (minute != 0 || second != 0 || ms != 0)) return nan_value();
    }

    bool tz_present = false;
    double tz_ms = 0.0;
    if (i < s.size()) {
        if (s[i] == 'Z') {
            ++i;
            tz_present = true;
        } else if (s[i] == '+' || s[i] == '-') {
            double sign = s[i] == '-' ? -1.0 : 1.0;
            ++i;
            long long oh, om = 0;
            if (!parse_fixed_digits(s, i, 2, oh) || oh > 23) return nan_value();
            if (i < s.size() && s[i] == ':') ++i;
            if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                if (!parse_fixed_digits(s, i, 2, om) || om > 59) return nan_value();
            }
            tz_present = true;
            tz_ms = sign * (oh * kMsPerHour + om * kMsPerMinute);
        }
    }
    if (i != s.size()) return nan_value();

    double d = make_day(year, static_cast<double>(month - 1), static_cast<double>(day));
    double tm = make_time(static_cast<double>(hour), static_cast<double>(minute),
                          static_cast<double>(second), static_cast<double>(ms));
    double dv = make_date(d, tm);
    if (tz_present) {
        dv -= tz_ms;
    } else if (!date_only) {
        dv = utc_time(dv);
    }
    return time_clip(dv);
}

int month_name_index(const std::string& tok) {
    for (int m = 0; m < 12; ++m) {
        if (tok == kMonthNames[m]) return m;
    }
    return -1;
}

bool is_day_name(const std::string& tok) {
    for (const char* d : kDayNames) {
        if (tok == d) return true;
    }
    return false;
}

// Accepts the formats this engine emits: toString ("Thu Jan 01 1970 00:00:00 GMT+0000 (Zone)"),
// toUTCString ("Thu, 01 Jan 1970 00:00:00 GMT") and toDateString ("Thu Jan 01 1970").
double parse_legacy_string(const std::string& input) {
    std::string s = input;
    size_t paren = s.find('(');
    if (paren != std::string::npos) s = s.substr(0, paren);

    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == '\t') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) return nan_value();

    int month = -1;
    bool have_day = false, have_year = false, have_time = false;
    bool tz_present = false;
    double day = 0, year = 0, tz_ms = 0.0;
    long long hour = 0, minute = 0, second = 0;

    for (const std::string& tok : tokens) {
        if (is_day_name(tok)) continue;
        int m = month_name_index(tok);
        if (m >= 0) {
            if (month >= 0) return nan_value();
            month = m;
            continue;
        }
        if (tok.find(':') != std::string::npos) {
            if (have_time) return nan_value();
            long long h, mi, se = 0;
            size_t p = 0;
            if (!parse_fixed_digits(tok, p, 2, h) || p >= tok.size() || tok[p] != ':') return nan_value();
            ++p;
            if (!parse_fixed_digits(tok, p, 2, mi)) return nan_value();
            if (p < tok.size()) {
                if (tok[p] != ':') return nan_value();
                ++p;
                if (!parse_fixed_digits(tok, p, 2, se) || p != tok.size()) return nan_value();
            }
            if (h > 23 || mi > 59 || se > 59) return nan_value();
            hour = h; minute = mi; second = se;
            have_time = true;
            continue;
        }
        if (tok == "GMT" || tok == "UTC" || tok == "UT" || tok == "Z") {
            tz_present = true;
            continue;
        }
        if (tok.rfind("GMT", 0) == 0 || tok.rfind("UTC", 0) == 0 ||
            tok[0] == '+' || (tok[0] == '-' && have_year)) {
            std::string off = tok;
            if (off.rfind("GMT", 0) == 0 || off.rfind("UTC", 0) == 0) off = off.substr(3);
            if (off.empty()) { tz_present = true; continue; }
            if (off[0] != '+' && off[0] != '-') return nan_value();
            double sign = off[0] == '-' ? -1.0 : 1.0;
            size_t p = 1;
            long long oh, om = 0;
            if (!parse_fixed_digits(off, p, 2, oh)) return nan_value();
            if (p < off.size() && off[p] == ':') ++p;
            if (p < off.size() && !parse_fixed_digits(off, p, 2, om)) return nan_value();
            if (p != off.size()) return nan_value();
            tz_present = true;
            tz_ms = sign * (oh * kMsPerHour + om * kMsPerMinute);
            continue;
        }
        {
            char* end = nullptr;
            long long n = std::strtoll(tok.c_str(), &end, 10);
            if (!end || *end != '\0' || end == tok.c_str()) return nan_value();
            if (!have_day && n >= 1 && n <= 31 && tok[0] != '-' && tok.size() <= 2) {
                day = static_cast<double>(n);
                have_day = true;
            } else if (!have_year) {
                year = static_cast<double>(n);
                have_year = true;
            } else if (!have_day && n >= 1 && n <= 31) {
                day = static_cast<double>(n);
                have_day = true;
            } else {
                return nan_value();
            }
        }
    }

    if (month < 0 || !have_day || !have_year) return nan_value();

    double d = make_day(year, month, day);
    double tm = make_time(static_cast<double>(hour), static_cast<double>(minute),
                          static_cast<double>(second), 0.0);
    double dv = make_date(d, tm);
    if (tz_present) {
        dv -= tz_ms;
    } else {
        dv = utc_time(dv);
    }
    return time_clip(dv);
}

double parse_date_string(const std::string& input) {
    size_t start = input.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return nan_value();
    size_t end = input.find_last_not_of(" \t\n\r");
    std::string s = input.substr(start, end - start + 1);

    bool iso_shaped = false;
    double iso = parse_iso_string(s, iso_shaped);
    if (iso_shaped) return iso;
    return parse_legacy_string(s);
}

Value date_getter(Context& ctx, bool utc, double (*extract)(double)) {
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) return nan_result();
    return Value(extract(utc ? t : local_time(t)));
}

} // namespace

double Date::current_time_ms() {
    auto now = std::chrono::system_clock::now();
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count());
}

std::string Date::to_date_string(double tv) {
    if (std::isnan(tv)) return "Invalid Date";
    double t = local_time(tv);
    return date_string(t) + " " + time_string(t) + time_zone_string(tv);
}

Value Date::date_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        return Value(to_date_string(current_time_ms()));
    }

    double dv;
    if (args.empty()) {
        dv = current_time_ms();
    } else if (args.size() == 1) {
        const Value& v = args[0];
        if (v.is_object() && v.as_object()->get_type() == Object::ObjectType::Date) {
            dv = get_date_value(v.as_object());
        } else {
            Value prim = to_primitive(ctx, v, "default");
            if (ctx.has_exception()) return Value();
            double tv;
            if (prim.is_string()) {
                tv = parse_date_string(prim.as_string()->str());
            } else {
                tv = prim.to_number();
                if (ctx.has_exception()) return Value();
            }
            dv = time_clip(tv);
        }
    } else {
        double f[7] = {0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
        for (size_t k = 0; k < 7 && k < args.size(); ++k) {
            f[k] = args[k].to_number();
            if (ctx.has_exception()) return Value();
        }
        double y = f[0];
        if (!std::isnan(y)) {
            double yi = to_integer(y);
            if (yi >= 0.0 && yi <= 99.0) y = 1900.0 + yi;
        }
        double final_date = make_date(make_day(y, f[1], f[2]), make_time(f[3], f[4], f[5], f[6]));
        dv = time_clip(utc_time(final_date));
    }

    auto obj = std::make_unique<Object>(Object::ObjectType::Date);
    obj->set_property("_isDate", Value(true), PropertyAttributes::Writable);
    obj->set_property("_timestamp", Value(dv), PropertyAttributes::Writable);
    return Value(obj.release());
}

Value Date::now(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;
    return Value(current_time_ms());
}

Value Date::parse(Context& ctx, const std::vector<Value>& args) {
    Value str_val = args.empty() ? Value() : args[0];
    std::string s = str_val.to_string();
    if (ctx.has_exception()) return Value();
    return Value(parse_date_string(s));
}

Value Date::UTC(Context& ctx, const std::vector<Value>& args) {
    double f[7] = {nan_value(), 0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
    f[0] = (args.empty() ? Value() : args[0]).to_number();
    if (ctx.has_exception()) return Value();
    for (size_t k = 1; k < 7 && k < args.size(); ++k) {
        f[k] = args[k].to_number();
        if (ctx.has_exception()) return Value();
    }
    double y = f[0];
    if (!std::isnan(y)) {
        double yi = to_integer(y);
        if (yi >= 0.0 && yi <= 99.0) y = 1900.0 + yi;
    }
    return Value(time_clip(make_date(make_day(y, f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}

Value Date::getTime(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    return Value(t);
}

Value Date::valueOf(Context& ctx, const std::vector<Value>& args) {
    return getTime(ctx, args);
}

Value Date::getFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return year_from_time(t); });
}

Value Date::getMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return static_cast<double>(month_from_time(t)); });
}

Value Date::getDate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return date_from_time(t); });
}

Value Date::getDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return week_day(t); });
}

Value Date::getHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return hour_from_time(t); });
}

Value Date::getMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return min_from_time(t); });
}

Value Date::getSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return sec_from_time(t); });
}

Value Date::getMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return ms_from_time(t); });
}

Value Date::getYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, false, [](double t) { return year_from_time(t) - 1900.0; });
}

Value Date::getUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return year_from_time(t); });
}

Value Date::getUTCMonth(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return static_cast<double>(month_from_time(t)); });
}

Value Date::getUTCDate(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return date_from_time(t); });
}

Value Date::getUTCDay(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return week_day(t); });
}

Value Date::getUTCHours(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return hour_from_time(t); });
}

Value Date::getUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return min_from_time(t); });
}

Value Date::getUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return sec_from_time(t); });
}

Value Date::getUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    return date_getter(ctx, true, [](double t) { return ms_from_time(t); });
}

Value Date::getTimezoneOffset(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) return nan_result();
    return Value((t - local_time(t)) / kMsPerMinute);
}

Value Date::setTime(Context& ctx, const std::vector<Value>& args) {
    Object* obj = this_date_object(ctx);
    if (!obj) return Value();
    double v = (args.empty() ? Value() : args[0]).to_number();
    if (ctx.has_exception()) return Value();
    double u = time_clip(v);
    set_date_value(obj, u);
    return Value(u);
}

Value Date::setMilliseconds(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldMs, 1);
}

Value Date::setSeconds(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldSec, 2);
}

Value Date::setMinutes(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldMin, 3);
}

Value Date::setHours(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldHour, 4);
}

Value Date::setDate(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldDate, 1);
}

Value Date::setMonth(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldMonth, 2);
}

Value Date::setFullYear(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, false, kFieldYear, 3);
}

Value Date::setUTCMilliseconds(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldMs, 1);
}

Value Date::setUTCSeconds(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldSec, 2);
}

Value Date::setUTCMinutes(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldMin, 3);
}

Value Date::setUTCHours(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldHour, 4);
}

Value Date::setUTCDate(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldDate, 1);
}

Value Date::setUTCMonth(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldMonth, 2);
}

Value Date::setUTCFullYear(Context& ctx, const std::vector<Value>& args) {
    return set_date_fields(ctx, args, true, kFieldYear, 3);
}

Value Date::setYear(Context& ctx, const std::vector<Value>& args) {
    Object* obj = this_date_object(ctx);
    if (!obj) return Value();
    double t = get_date_value(obj);
    t = std::isnan(t) ? 0.0 : local_time(t);
    double y = (args.empty() ? Value() : args[0]).to_number();
    if (ctx.has_exception()) return Value();
    if (std::isnan(y)) {
        set_date_value(obj, nan_value());
        return nan_result();
    }
    double yi = to_integer(y);
    double yyyy = (yi >= 0.0 && yi <= 99.0) ? yi + 1900.0 : y;
    double d = make_day(yyyy, month_from_time(t), date_from_time(t));
    double u = time_clip(utc_time(make_date(d, time_within_day(t))));
    set_date_value(obj, u);
    return Value(u);
}

Value Date::toString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    return Value(to_date_string(t));
}

Value Date::toDateString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) return Value(std::string("Invalid Date"));
    return Value(date_string(local_time(t)));
}

Value Date::toTimeString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) return Value(std::string("Invalid Date"));
    return Value(time_string(local_time(t)) + time_zone_string(t));
}

Value Date::toISOString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) {
        ctx.throw_range_error("Invalid time value");
        return Value();
    }
    double y = year_from_time(t);
    char ybuf[16];
    if (y >= 0.0 && y <= 9999.0) {
        std::snprintf(ybuf, sizeof ybuf, "%04d", static_cast<int>(y));
    } else {
        std::snprintf(ybuf, sizeof ybuf, "%c%06lld", y < 0 ? '-' : '+',
                      static_cast<long long>(std::fabs(y)));
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s-%02d-%02dT%02d:%02d:%02d.%03dZ", ybuf,
                  month_from_time(t) + 1,
                  static_cast<int>(date_from_time(t)),
                  static_cast<int>(hour_from_time(t)),
                  static_cast<int>(min_from_time(t)),
                  static_cast<int>(sec_from_time(t)),
                  static_cast<int>(ms_from_time(t)));
    return Value(std::string(buf));
}

Value Date::toUTCString(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    double t;
    if (!this_time_value(ctx, t)) return Value();
    if (std::isnan(t)) return Value(std::string("Invalid Date"));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %02d %s ",
                  kDayNames[static_cast<int>(week_day(t))],
                  static_cast<int>(date_from_time(t)),
                  kMonthNames[month_from_time(t)]);
    return Value(buf + padded_year(year_from_time(t)) + " " + time_string(t));
}

Value Date::toGMTString(Context& ctx, const std::vector<Value>& args) {
    return toUTCString(ctx, args);
}

Value Date::toLocaleString(Context& ctx, const std::vector<Value>& args) {
    return toString(ctx, args);
}

Value Date::toLocaleDateString(Context& ctx, const std::vector<Value>& args) {
    return toDateString(ctx, args);
}

Value Date::toLocaleTimeString(Context& ctx, const std::vector<Value>& args) {
    return toTimeString(ctx, args);
}

Value Date::toJSON(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    if (ctx.original_this_was_nullish()) {
        ctx.throw_type_error("Date.prototype.toJSON called on null or undefined");
        return Value();
    }

    Value this_val;
    if (ctx.original_this_was_primitive()) {
        try {
            this_val = ctx.get_binding("this");
        } catch (...) {
            ctx.throw_type_error("Date.prototype.toJSON called on invalid this");
            return Value();
        }
        this_val = ObjectFactory::box_primitive_this_sloppy(ctx, this_val);
        if (ctx.has_exception()) return Value();
    } else {
        Object* obj = ctx.get_this_binding();
        if (!obj) {
            ctx.throw_type_error("Date.prototype.toJSON called on null or undefined");
            return Value();
        }
        this_val = Value(obj);
    }
    if (!this_val.is_object() && !this_val.is_function()) {
        ctx.throw_type_error("Date.prototype.toJSON called on invalid this");
        return Value();
    }
    Object* obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function())
                                         : this_val.as_object();

    Value tv = to_primitive(ctx, this_val, "number");
    if (ctx.has_exception()) return Value();
    if (tv.is_number() && !std::isfinite(tv.as_number())) return Value::null();

    Value to_iso = obj->get_property("toISOString");
    if (ctx.has_exception()) return Value();
    if (!to_iso.is_function()) {
        ctx.throw_type_error("toISOString is not a function");
        return Value();
    }
    return to_iso.as_function()->call(ctx, {}, this_val);
}

Value Date::symbol_to_primitive(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj || ctx.original_this_was_primitive() || ctx.original_this_was_nullish()) {
        ctx.throw_type_error("Date.prototype[Symbol.toPrimitive] called on non-object");
        return Value();
    }
    bool prefer_string;
    if (!args.empty() && args[0].is_string()) {
        const std::string& hint = args[0].as_string()->str();
        if (hint == "string" || hint == "default") {
            prefer_string = true;
        } else if (hint == "number") {
            prefer_string = false;
        } else {
            ctx.throw_type_error("Invalid hint for Date[Symbol.toPrimitive]");
            return Value();
        }
    } else {
        ctx.throw_type_error("Invalid hint for Date[Symbol.toPrimitive]");
        return Value();
    }
    return ordinary_to_primitive(ctx, obj, prefer_string);
}

}
