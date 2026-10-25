/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlDateTime.h"

#include "quanta/core/runtime/Symbol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace Quanta::IntlImpl {

namespace {

// ---- Time zones -------------------------------------------------------------------------------------------------------

// The zone the host runs in, as an IANA name.
std::string system_time_zone() {
#ifdef _WIN32
    // Windows names its zones its own way; the offset of the local zone is still read from the C library below.
    return "UTC";
#else
    static const std::string zone = [] {
        if (const char* tz = std::getenv("TZ"); tz && *tz) {
            std::string name = tz[0] == ':' ? tz + 1 : tz;
            if (!find_time_zone(name).empty()) return find_time_zone(name);
        }
        char buffer[512];
        const ssize_t length = readlink("/etc/localtime", buffer, sizeof(buffer) - 1);
        if (length > 0) {
            std::string target(buffer, static_cast<size_t>(length));
            const size_t at = target.find("zoneinfo/");
            if (at != std::string::npos) {
                const std::string name = find_time_zone(target.substr(at + 9));
                if (!name.empty()) return name;
            }
        }
        return std::string("UTC");
    }();
    return zone;
#endif
}

// An offset zone such as "+05:30" or "-0800" as seconds from UTC.
bool parse_offset_zone(std::string_view s, int& seconds, std::string& normalized) {
    if (s.size() < 3 || (s[0] != '+' && s[0] != '-')) return false;
    auto digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
    if (!digit(1) || !digit(2)) return false;
    const int hours = (s[1] - '0') * 10 + (s[2] - '0');
    int minutes = 0;
    size_t at = 3;
    if (at < s.size()) {
        if (s[at] == ':') at++;
        if (!digit(at) || !digit(at + 1)) return false;
        minutes = (s[at] - '0') * 10 + (s[at + 1] - '0');
        at += 2;
    }
    if (at != s.size() || hours > 23 || minutes > 59) return false;
    seconds = (hours * 60 + minutes) * 60 * (s[0] == '-' ? -1 : 1);
    char text[8];
    std::snprintf(text, sizeof(text), "%c%02d:%02d", s[0], hours, minutes);
    normalized = text;
    return true;
}

// The offset a zone has from UTC at a moment (seconds), asking the host's zone database for named zones.
int zone_offset_seconds(const std::string& zone, double utc_ms) {
    int fixed;
    std::string normalized;
    if (zone == "UTC" || zone == "Etc/UTC" || zone == "GMT" || zone == "Etc/GMT" || zone == "Zulu" || zone == "UCT") return 0;
    if (parse_offset_zone(zone, fixed, normalized)) return fixed;
    if (zone.compare(0, 7, "Etc/GMT") == 0 && zone.size() > 7) return -std::atoi(zone.c_str() + 7) * 3600;
    const std::time_t seconds = static_cast<std::time_t>(std::floor(utc_ms / 1000.0));
    std::tm local{};
#ifdef _WIN32
    // Only the host's own zone is known here: its offset from the difference between local and UTC readings.
    std::tm utc{};
    if (localtime_s(&local, &seconds) != 0 || gmtime_s(&utc, &seconds) != 0) return 0;
    utc.tm_isdst = local.tm_isdst;
    return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)));
#else
    if (zone == system_time_zone()) {
        if (localtime_r(&seconds, &local) == nullptr) return 0;
        return static_cast<int>(local.tm_gmtoff);
    }
    // Another zone: borrow the host's database by pointing TZ at it for the length of the call.
    static thread_local std::string saved;
    const char* previous = std::getenv("TZ");
    saved = previous ? previous : "";
    setenv("TZ", zone.c_str(), 1);
    tzset();
    const bool ok = localtime_r(&seconds, &local) != nullptr;
    if (previous) setenv("TZ", saved.c_str(), 1);
    else unsetenv("TZ");
    tzset();
    return ok ? static_cast<int>(local.tm_gmtoff) : 0;
#endif
}

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
void civil_from_days(int64_t z, int& year, int& month, int& day) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(y + (month <= 2));
}

struct Broken {
    int year, month, day, hour, minute, second, millisecond;
};

Broken break_down(double local_ms) {
    const double day_ms = 86400000.0;
    const double days = std::floor(local_ms / day_ms);
    double rest = local_ms - days * day_ms;
    Broken b{};
    civil_from_days(static_cast<int64_t>(days), b.year, b.month, b.day);
    b.hour = static_cast<int>(rest / 3600000.0);
    rest -= b.hour * 3600000.0;
    b.minute = static_cast<int>(rest / 60000.0);
    rest -= b.minute * 60000.0;
    b.second = static_cast<int>(rest / 1000.0);
    b.millisecond = static_cast<int>(rest - b.second * 1000.0);
    return b;
}

}  // namespace

std::string DateTimeFormatObject::skeleton() const {
    std::string s;
    auto width = [](const std::string& value, const char* numeric, const char* two_digit, const char* narrow, const char* short_form,
                    const char* long_form) -> std::string {
        if (value == "numeric") return numeric;
        if (value == "2-digit") return two_digit;
        if (value == "narrow") return narrow;
        if (value == "short") return short_form;
        if (value == "long") return long_form;
        return "";
    };
    if (!date_style.empty() || !time_style.empty()) {
        if (date_style == "full") s += "yMMMMEEEEd";
        else if (date_style == "long") s += "yMMMMd";
        else if (date_style == "medium") s += "yMMMd";
        else if (date_style == "short") s += "yyMd";
        const char* hour_letter = (hour_cycle == "h11" || hour_cycle == "h12") ? "h" : "H";
        if (time_style == "full") s += std::string(hour_letter) + "mszzzz";
        else if (time_style == "long") s += std::string(hour_letter) + "msz";
        else if (time_style == "medium") s += std::string(hour_letter) + "ms";
        else if (time_style == "short") s += std::string(hour_letter) + "m";
        return s;
    }
    s += width(era, "G", "G", "GGGGG", "G", "GGGG");
    s += width(year, "y", "yy", "", "", "");
    s += width(month, "M", "MM", "MMMMM", "MMM", "MMMM");
    s += width(day, "d", "dd", "", "", "");
    s += width(weekday, "", "", "EEEEE", "E", "EEEE");
    s += width(day_period, "", "", "BBBBB", "B", "BBBB");
    if (!hour.empty()) {
        const char* letter = (hour_cycle == "h11" || hour_cycle == "h12") ? "h" : "H";
        s += hour == "2-digit" ? std::string(letter) + letter : std::string(letter);
    }
    s += width(minute, "m", "mm", "", "", "");
    s += width(second, "s", "ss", "", "", "");
    if (fractional_digits > 0) s += std::string(static_cast<size_t>(fractional_digits), 'S');
    if (time_zone_name == "short") s += "z";
    else if (time_zone_name == "long") s += "zzzz";
    else if (time_zone_name == "shortOffset") s += "O";
    else if (time_zone_name == "longOffset") s += "OOOO";
    else if (time_zone_name == "shortGeneric") s += "v";
    else if (time_zone_name == "longGeneric") s += "vvvv";
    return s;
}

bool DateTimeFormatObject::format(double t, std::vector<DateTimePart>& parts) const {
    const int offset = zone_offset_seconds(time_zone, t);
    Broken b = break_down(t + offset * 1000.0);
    // The calendar library stops at year 9999; a 400-year cycle is a whole number of weeks, so the same day of a year
    // inside the range reads the same, and the year is written in afterwards.
    int shown_year = 0;
    if (b.year > 9000 || b.year < -9000) {
        const int display = b.year > 0 ? b.year : 1 - b.year;
        shown_year = display;
        const int stand_in = 2000 + display % 400;
        b.year = b.year > 0 ? stand_in : 1 - stand_in;
    }
    const std::string language = IntlICU::split_tag(locale).language;
    const bool has_date = !era.empty() || !year.empty() || !month.empty() || !day.empty() || !weekday.empty();
    const bool plain_time = date_style.empty() && time_style.empty() && !has_date && hour.empty() && time_zone_name.empty();
    // English writes the periods of the day by their hours; the data's own day periods are only am and pm.
    auto flexible_period = [&]() -> std::string {
        if (b.hour == 12 && b.minute == 0 && b.second == 0) return day_period == "narrow" ? "n" : "noon";
        if (b.hour >= 6 && b.hour < 12) return "in the morning";
        if (b.hour >= 12 && b.hour < 18) return "in the afternoon";
        if (b.hour >= 18 && b.hour < 21) return "in the evening";
        return "at night";
    };
    if (plain_time && !day_period.empty() && minute.empty() && second.empty() && language == "en") {
        parts.push_back({"dayPeriod", flexible_period()});
        return true;
    }
    if (plain_time && day_period.empty() && (!minute.empty() || !second.empty() || fractional_digits > 0)) {
        // The calendar library has no format with minutes and seconds but no hour: assembled here from the digits.
        const Symbols symbols = learn_symbols(icu_locale);
        auto number = [&](int value, bool two_digits) {
            std::string text;
            const std::string digits = std::to_string(value);
            for (size_t i = digits.size(); two_digits && i < 2; i++) text += symbols.digit[0];
            for (char c : digits) text += symbols.digit[c - '0'];
            return text;
        };
        if (!minute.empty()) parts.push_back({"minute", number(b.minute, minute == "2-digit" || !second.empty())});
        if (!second.empty()) {
            if (!parts.empty()) parts.push_back({"literal", ":"});
            parts.push_back({"second", number(b.second, second == "2-digit" || !minute.empty())});
        }
        if (fractional_digits > 0) {
            if (!parts.empty()) parts.push_back({"literal", symbols.decimal.empty() ? "." : symbols.decimal});
            const std::string milli = number(b.millisecond, false);
            std::string padded;
            const std::string ascii = std::to_string(1000 + b.millisecond).substr(1);
            for (int i = 0; i < fractional_digits; i++) padded += symbols.digit[ascii[i] - '0'];
            parts.push_back({"fractionalSecond", padded});
            (void)milli;
        }
        return true;
    }
    const bool named = !(time_zone.size() > 0 && (time_zone[0] == '+' || time_zone[0] == '-'));
    std::optional<std::string> raw = IntlICU::shim::datetime(icu_locale, skeleton(), b.year, b.month, b.day, b.hour, b.minute,
                                                              b.second, b.millisecond * 1000000, named ? time_zone : std::string(), offset);
    if (!raw) return false;
    const size_t nul = raw->find('\0');
    const std::string text = raw->substr(0, nul);
    struct Raw { std::string name; size_t start, end; };
    std::vector<Raw> found;
    for (size_t at = nul + 1; at < raw->size();) {
        const size_t semicolon = raw->find(';', at);
        const std::string item = raw->substr(at, semicolon - at);
        const size_t c1 = item.find(','), c2 = item.find(',', c1 + 1);
        const std::string name = item.substr(0, c1);
        at = semicolon + 1;
        static const char* const known[] = {"era", "year", "relatedYear", "yearName", "month", "day", "weekday", "dayPeriod",
                                            "hour", "minute", "second", "timeZoneName"};
        if (std::find(std::begin(known), std::end(known), name) == std::end(known)) continue;
        found.push_back({name, static_cast<size_t>(std::stoul(item.substr(c1 + 1, c2 - c1 - 1))),
                         static_cast<size_t>(std::stoul(item.substr(c2 + 1)))});
    }
    std::sort(found.begin(), found.end(), [](const Raw& a, const Raw& b) { return a.start < b.start; });
    size_t cursor = 0;
    for (const Raw& f : found) {
        if (f.start < cursor) continue;
        if (f.start > cursor) parts.push_back({"literal", text.substr(cursor, f.start - cursor)});
        std::string value = text.substr(f.start, f.end - f.start);
        if (f.name == "second" && fractional_digits > 0 && value.size() > static_cast<size_t>(fractional_digits)) {
            // The seconds come with their fraction ("09.123"): the last digits are the fractionalSecond.
            size_t cut = value.size();
            int digits = 0;
            while (cut > 0 && digits < fractional_digits) {
                cut--;
                while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) cut--;
                digits++;
            }
            size_t separator = cut;
            while (separator > 0 && !(value[separator - 1] >= '0' && value[separator - 1] <= '9') &&
                   (static_cast<unsigned char>(value[separator - 1]) < 0x80)) separator--;
            parts.push_back({"second", value.substr(0, separator)});
            parts.push_back({"literal", value.substr(separator, cut - separator)});
            parts.push_back({"fractionalSecond", value.substr(cut)});
        } else if (f.name == "year" && shown_year != 0 && year != "2-digit") {
            std::string digits = std::to_string(shown_year);
            if (numbering_system != "latn") {
                const std::string_view system = numbering_system_digits(numbering_system);
                std::string translated;
                for (char c : digits) {
                    size_t at = 0;
                    for (int d = 0; d < 10; d++) {
                        const unsigned char lead = static_cast<unsigned char>(system[at]);
                        const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                        if (d == c - '0') { translated += std::string(system.substr(at, length)); break; }
                        at += length;
                    }
                }
                if (!translated.empty()) digits = translated;
            }
            parts.push_back({"year", digits});
        } else if (f.name == "dayPeriod" && language == "en" && !day_period.empty()) {
            // The pattern's narrow no-break space belongs to am and pm; the words take an ordinary one.
            if (!parts.empty() && parts.back().type == "literal" && parts.back().value == "\xE2\x80\xAF") parts.back().value = " ";
            parts.push_back({"dayPeriod", flexible_period()});
        } else {
            parts.push_back({f.name, std::move(value)});
        }
        cursor = f.end;
    }
    if (cursor < text.size()) parts.push_back({"literal", text.substr(cursor)});
    return true;
}

namespace {

// ---- Construction ---------------------------------------------------------------------------------------------------------

std::vector<std::string> calendar_values(const std::string& data_locale) {
    std::vector<std::string> values;
    LocaleId id = IntlICU::split_tag(data_locale);
    const std::vector<std::string> supported = supported_values("calendar");
    std::string preferred = "gregory";
    // The calendar the locale's region prefers, when we have it.
    if (!id.region.empty() || id.language == "th") {
        if (id.language == "th") preferred = "buddhist";
        else if (id.region == "TH") preferred = "buddhist";
        else if (id.region == "IR" || id.region == "AF") preferred = "persian";
        else if (id.region == "TW") preferred = "gregory";
    }
    values.push_back(preferred);
    for (const std::string& c : supported) {
        if (c != preferred) values.push_back(c);
    }
    return values;
}

std::vector<std::string> hour_cycle_values(const std::string&) { return {"h12", "h23", "h11", "h24"}; }

// The clock the locale writes by default: its time pattern for "j" has a day period when it is a 12-hour one.
std::string default_hour_cycle(const std::string& locale) {
    LocaleId id = IntlICU::split_tag(locale);
    id.keywords.clear();
    std::optional<std::string> raw = IntlICU::shim::datetime(id.to_string(), "j", 2000, 1, 1, 13, 0, 0, 0, "", 0);
    if (!raw) return "h23";
    const bool day_period = raw->find("dayPeriod,", raw->find('\0')) != std::string::npos;
    if (!day_period) return "h23";
    return id.language == "ja" ? "h11" : "h12";
}

}  // namespace

// ---- Options ----------------------------------------------------------------------------------------------------------------

// CreateDateTimeFormat. `required` and `defaults` are ToDateTimeOptions' ("date", "time", "any" / "date", "time", "all").
DateTimeFormatObject* create_date_time_format(Context& ctx, const Value& locales, const Value& options_value, const char* required,
                                              const char* defaults, Object* proto) {
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, locales, requested)) return nullptr;
    Object* options = coerce_options_to_object(ctx, options_value);
    if (!options) return nullptr;
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return nullptr;
    auto type_text = [&](const char* name, std::optional<std::string>& out) {
        if (!get_string_option(ctx, options, name, {}, out)) return false;
        if (!out) return true;
        bool valid = !out->empty();
        size_t start = 0;
        while (valid) {
            size_t dash = out->find('-', start);
            size_t end = dash == std::string::npos ? out->size() : dash;
            if (end - start < 3 || end - start > 8) valid = false;
            for (size_t i = start; valid && i < end; i++) valid = std::isalnum(static_cast<unsigned char>((*out)[i])) != 0;
            if (dash == std::string::npos) break;
            start = dash + 1;
        }
        if (!valid) return range_error(ctx, std::string("Invalid ") + name);
        for (char& c : *out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return true;
    };
    std::optional<std::string> calendar, numbering;
    if (!type_text("calendar", calendar)) return nullptr;
    if (!type_text("numberingSystem", numbering)) return nullptr;
    if (calendar) {
        if (*calendar == "gregorian") *calendar = "gregory";
        else if (*calendar == "islamicc") *calendar = "islamic-civil";
        else if (*calendar == "ethiopic-amete-alem") *calendar = "ethioaa";
    }
    std::optional<bool> hour12;
    if (!get_boolean_option(ctx, options, "hour12", hour12)) return nullptr;
    std::optional<std::string> hour_cycle_option;
    if (!get_string_option(ctx, options, "hourCycle", {"h11", "h12", "h23", "h24"}, hour_cycle_option)) return nullptr;
    if (hour12) hour_cycle_option.reset();

    std::vector<std::pair<std::string, std::string>> overrides;
    if (calendar) overrides.emplace_back("ca", *calendar);
    if (numbering) overrides.emplace_back("nu", *numbering);
    if (hour_cycle_option) overrides.emplace_back("hc", *hour_cycle_option);
    Resolved resolved = resolve_locale(requested, {{"ca", calendar_values}, {"hc", hour_cycle_values}, {"nu", numbering_system_values}},
                                       overrides, matcher);

    // timeZone
    std::string time_zone;
    {
        Value value = options->get_property("timeZone");
        if (ctx.has_exception()) return nullptr;
        if (value.is_undefined()) {
            time_zone = system_time_zone();
        } else {
            std::string text;
            if (!value.to_string_checked(ctx, text)) return nullptr;
            int seconds;
            std::string normalized;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                if (!parse_offset_zone(text, seconds, normalized)) return range_error(ctx, "Invalid time zone: " + text), nullptr;
                time_zone = normalized;
            } else {
                std::string name = find_time_zone(text);
                if (name.empty()) {
                    // Offset names written in IANA style are not zones.
                    return range_error(ctx, "Invalid time zone specified: " + text), nullptr;
                }
                time_zone = name;
            }
        }
    }

    // The components, in the order the specification reads them.
    struct Field { const char* name; std::initializer_list<const char*> allowed; std::optional<std::string> value; };
    Field fields[] = {
        {"weekday", {"narrow", "short", "long"}, {}}, {"era", {"narrow", "short", "long"}, {}},
        {"year", {"2-digit", "numeric"}, {}}, {"month", {"2-digit", "numeric", "narrow", "short", "long"}, {}},
        {"day", {"2-digit", "numeric"}, {}}, {"dayPeriod", {"narrow", "short", "long"}, {}},
        {"hour", {"2-digit", "numeric"}, {}}, {"minute", {"2-digit", "numeric"}, {}}, {"second", {"2-digit", "numeric"}, {}},
    };
    for (Field& f : fields) {
        if (!get_string_option(ctx, options, f.name, f.allowed, f.value)) return nullptr;
    }
    std::optional<double> fractional;
    if (!get_number_option(ctx, options, "fractionalSecondDigits", 1, 3, std::nullopt, fractional)) return nullptr;
    std::optional<std::string> zone_name, format_matcher, date_style, time_style;
    if (!get_string_option(ctx, options, "timeZoneName", {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"}, zone_name)) return nullptr;
    if (!get_string_option(ctx, options, "formatMatcher", {"basic", "best fit"}, format_matcher)) return nullptr;
    if (!get_string_option(ctx, options, "dateStyle", {"full", "long", "medium", "short"}, date_style)) return nullptr;
    if (!get_string_option(ctx, options, "timeStyle", {"full", "long", "medium", "short"}, time_style)) return nullptr;

    auto& weekday = fields[0].value; auto& era = fields[1].value; auto& year = fields[2].value; auto& month = fields[3].value;
    auto& day = fields[4].value; auto& day_period = fields[5].value; auto& hour = fields[6].value; auto& minute = fields[7].value;
    auto& second = fields[8].value;
    const bool explicit_fields = weekday || era || year || month || day || day_period || hour || minute || second || fractional || zone_name;
    if ((date_style || time_style) && explicit_fields) {
        type_error(ctx, "dateStyle and timeStyle can not be used with other date/time options");
        return nullptr;
    }
    const std::string need = required;
    if (need == "date" && time_style) { type_error(ctx, "timeStyle is not allowed here"); return nullptr; }
    if (need == "time" && date_style) { type_error(ctx, "dateStyle is not allowed here"); return nullptr; }
    if (!date_style && !time_style) {
        bool need_defaults = true;
        if ((need == "date" || need == "any") && (weekday || year || month || day)) need_defaults = false;
        if ((need == "time" || need == "any") && (day_period || hour || minute || second || fractional)) need_defaults = false;
        if (need_defaults && (std::string(defaults) == "date" || std::string(defaults) == "all")) {
            year = "numeric"; month = "numeric"; day = "numeric";
        }
        if (need_defaults && (std::string(defaults) == "time" || std::string(defaults) == "all")) {
            hour = "numeric"; minute = "numeric"; second = "numeric";
        }
    }

    auto* object = Heap::Allocate<DateTimeFormatObject>();
    DateTimeFormatObject& f = *object;
    f.locale = resolved.locale;
    f.data_locale = resolved.data_locale;
    f.calendar = *resolved.value_of("ca");
    f.numbering_system = *resolved.value_of("nu");
    f.time_zone = time_zone;
    f.weekday = weekday.value_or(""); f.era = era.value_or(""); f.year = year.value_or(""); f.month = month.value_or("");
    f.day = day.value_or(""); f.day_period = day_period.value_or(""); f.hour = hour.value_or(""); f.minute = minute.value_or("");
    f.second = second.value_or(""); f.fractional_digits = fractional ? static_cast<int>(*fractional) : 0;
    f.time_zone_name = zone_name.value_or(""); f.date_style = date_style.value_or(""); f.time_style = time_style.value_or("");

    // hourCycle: only when the format shows an hour.
    const bool shows_hour = !f.hour.empty() || !f.time_style.empty();
    if (shows_hour) {
        std::string preferred = *resolved.value_of("hc");
        const bool from_locale_keyword = IntlICU::split_tag(requested.empty() ? f.locale : requested[0]).keyword("hc") != nullptr;
        if (hour12) {
            preferred = *hour12 ? (IntlICU::split_tag(f.locale).language == "ja" ? "h11" : "h12") : "h23";
        } else if (!hour_cycle_option && !from_locale_keyword) {
            preferred = default_hour_cycle(f.locale);
        }
        f.hour_cycle = preferred;
    }
    // With an hour the minutes and seconds are always written with two digits.
    if (!f.hour.empty() && f.minute == "numeric") f.minute = "2-digit";
    if (!f.minute.empty() && f.second == "numeric") f.second = "2-digit";

    // The locale the formatter reads its calendar, digits and hour cycle from.
    LocaleId id = IntlICU::split_tag(f.locale);
    id.keywords.clear();
    id.keywords.emplace_back("ca", f.calendar);
    // The calendar library has no h11 and h24 patterns: they are shown as the h12 and h23 they differ from at midnight and noon only.
    if (!f.hour_cycle.empty()) id.keywords.emplace_back("hc", f.hour_cycle == "h11" ? "h12" : f.hour_cycle == "h24" ? "h23" : f.hour_cycle);
    id.keywords.emplace_back("nu", f.numbering_system);
    std::sort(id.keywords.begin(), id.keywords.end());
    f.icu_locale = id.to_string();
    if (proto) f.initialize_prototype(proto);
    return object;
}

namespace {

bool inherits_from(Object* object, Object* proto) {
    for (Object* p = object ? object->get_prototype() : nullptr; p; p = p->get_prototype()) {
        if (p == proto) return true;
    }
    return false;
}

Value chain_legacy(Context& ctx, const Value& this_value, DateTimeFormatObject* created) {
    Object* self = this_value.is_function() ? static_cast<Object*>(this_value.as_function()) : this_value.as_object_or_null();
    Object* proto = prototype_of(ctx, Kind::DateTimeFormat);
    if (self && proto && inherits_from(self, proto)) {
        PropertyDescriptor descriptor(Value(static_cast<Object*>(created)), static_cast<PropertyAttributes>(0));
        self->set_property_descriptor(legacy_fallback_symbol(ctx)->to_property_key(), descriptor);
        return this_value;
    }
    return Value(static_cast<Object*>(created));
}

DateTimeFormatObject* unwrap(Context& ctx, const Value& receiver) {
    if (DateTimeFormatObject* direct = DOMObject::Cast<DateTimeFormatObject>(receiver)) return direct;
    Object* self = receiver.is_function() ? static_cast<Object*>(receiver.as_function()) : receiver.as_object_or_null();
    Object* proto = prototype_of(ctx, Kind::DateTimeFormat);
    if (self && proto && inherits_from(self, proto)) {
        Value inner = self->get_property(legacy_fallback_symbol(ctx)->to_property_key());
        if (ctx.has_exception()) return nullptr;
        if (DateTimeFormatObject* found = DOMObject::Cast<DateTimeFormatObject>(inner)) return found;
    }
    ctx.throw_type_error("Method called on an incompatible receiver, not an Intl.DateTimeFormat");
    return nullptr;
}

Value construct_date_time_format(Context& ctx, std::span<const Value> args, Value this_value, bool is_construct, Value new_target) {
    Object* proto = prototype_from_new_target(ctx, is_construct ? new_target : Value(), Kind::DateTimeFormat);
    if (ctx.has_exception()) return Value();
    DateTimeFormatObject* created = create_date_time_format(ctx, args.empty() ? Value() : args[0], args.size() > 1 ? args[1] : Value(),
                                                            "any", "date", proto);
    if (!created) return Value();
    if (!is_construct) return chain_legacy(ctx, this_value, created);
    return Value(static_cast<Object*>(created));
}

// The time value an argument stands for: now for undefined, otherwise ToNumber then TimeClip. False with an exception.
bool time_value(Context& ctx, const Value& argument, double& t) {
    if (argument.is_undefined()) {
        t = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        return true;
    }
    if (!argument.to_number_checked(ctx, t)) return false;
    if (!std::isfinite(t) || std::fabs(t) > 8.64e15) return range_error(ctx, "Invalid time value");
    t = std::trunc(t) + 0.0;
    return true;
}

Value parts_to_array(const std::vector<DateTimePart>& parts, bool with_source) {
    auto array = ObjectFactory::create_array(0);
    uint32_t index = 0;
    for (const DateTimePart& p : parts) {
        Object* entry = make_object({{"type", Value(p.type)}, {"value", Value(p.value)}});
        if (with_source) put(entry, "source", Value(p.source));
        array->set_element(index++, Value(entry));
    }
    return Value(array.release());
}

std::string join_parts(const std::vector<DateTimePart>& parts) {
    std::string text;
    for (const DateTimePart& p : parts) text += p.value;
    return text;
}

bool range_parts(Context& ctx, const DateTimeFormatObject& self, std::span<const Value> args, std::vector<DateTimePart>& out) {
    const Value start = args.size() > 0 ? args[0] : Value();
    const Value end = args.size() > 1 ? args[1] : Value();
    if (start.is_undefined() || end.is_undefined()) return type_error(ctx, "startDate and endDate must be provided");
    double x, y;
    if (!time_value(ctx, start, x) || !time_value(ctx, end, y)) return false;
    std::vector<DateTimePart> a, b;
    if (!self.format(x, a) || !self.format(y, b)) return range_error(ctx, "Date formatting is not available");
    if (join_parts(a) == join_parts(b)) {
        for (DateTimePart& p : a) { p.source = "shared"; out.push_back(std::move(p)); }
        return true;
    }
    for (DateTimePart& p : a) { p.source = "startRange"; out.push_back(std::move(p)); }
    out.push_back({"literal", "\xE2\x80\x89\xE2\x80\x93\xE2\x80\x89", "shared"});
    for (DateTimePart& p : b) { p.source = "endRange"; out.push_back(std::move(p)); }
    return true;
}

}  // namespace

void register_date_time_format(Context& ctx, Object* intl) {
    Class format = define_class(ctx, intl, "DateTimeFormat", 0, Kind::DateTimeFormat, construct_date_time_format);
    add_supported_locales_of(ctx, format.constructor);
    Object* proto = format.prototype;

    define_getter(proto, "format", [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        DateTimeFormatObject* self = unwrap(ctx, receiver);
        if (!self) return Value();
        if (self->bound_format.Get().is_undefined()) {
            self->bound_format.Set(make_bound_function(ctx, self, 1,
                [](Context& ctx, std::span<const Value> args, DOMObject* owner) -> Value {
                    double t;
                    if (!time_value(ctx, args.empty() ? Value() : args[0], t)) return Value();
                    std::vector<DateTimePart> parts;
                    if (!static_cast<DateTimeFormatObject*>(owner)->format(t, parts)) return range_error(ctx, "Date formatting is not available"), Value();
                    return Value(join_parts(parts));
                }));
        }
        return self->bound_format.Get();
    });

    define_method(proto, "formatToParts", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DateTimeFormatObject>(ctx, receiver, "DateTimeFormat");
        if (!self) return Value();
        double t;
        if (!time_value(ctx, args.empty() ? Value() : args[0], t)) return Value();
        std::vector<DateTimePart> parts;
        if (!self->format(t, parts)) return range_error(ctx, "Date formatting is not available"), Value();
        return parts_to_array(parts, false);
    });

    define_method(proto, "formatRange", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DateTimeFormatObject>(ctx, receiver, "DateTimeFormat");
        if (!self) return Value();
        std::vector<DateTimePart> parts;
        if (!range_parts(ctx, *self, args, parts)) return Value();
        return Value(join_parts(parts));
    });

    define_method(proto, "formatRangeToParts", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DateTimeFormatObject>(ctx, receiver, "DateTimeFormat");
        if (!self) return Value();
        std::vector<DateTimePart> parts;
        if (!range_parts(ctx, *self, args, parts)) return Value();
        return parts_to_array(parts, true);
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        DateTimeFormatObject* self = unwrap(ctx, receiver);
        if (!self) return Value();
        const DateTimeFormatObject& f = *self;
        auto result = ObjectFactory::create_object();
        auto add = [&](const char* name, const std::string& value) { if (!value.empty()) put(result.get(), name, Value(value)); };
        put(result.get(), "locale", Value(f.locale));
        put(result.get(), "calendar", Value(f.calendar));
        put(result.get(), "numberingSystem", Value(f.numbering_system));
        put(result.get(), "timeZone", Value(f.time_zone));
        if (!f.hour_cycle.empty()) {
            put(result.get(), "hourCycle", Value(f.hour_cycle));
            put(result.get(), "hour12", Value(f.hour_cycle == "h11" || f.hour_cycle == "h12"));
        }
        add("weekday", f.weekday); add("era", f.era); add("year", f.year); add("month", f.month); add("day", f.day);
        add("dayPeriod", f.day_period); add("hour", f.hour); add("minute", f.minute); add("second", f.second);
        if (f.fractional_digits > 0) put(result.get(), "fractionalSecondDigits", Value(static_cast<double>(f.fractional_digits)));
        add("timeZoneName", f.time_zone_name); add("dateStyle", f.date_style); add("timeStyle", f.time_style);
        return Value(result.release());
    });
}

bool date_to_locale_string(Context& ctx, double time_value, const Value& locales, const Value& options, const char* required,
                           const char* defaults, std::string& out) {
    Object* proto = prototype_of(ctx, Kind::DateTimeFormat);
    DateTimeFormatObject* format = create_date_time_format(ctx, locales, options, required, defaults, proto);
    if (!format) return false;
    std::vector<DateTimePart> parts;
    if (!format->format(time_value, parts)) return range_error(ctx, "Date formatting is not available");
    out = join_parts(parts);
    return true;
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
