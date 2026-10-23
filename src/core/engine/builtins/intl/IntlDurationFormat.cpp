/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace Quanta::IntlImpl {

namespace {

constexpr size_t kUnits = 10;
const char* const kUnitNames[kUnits] = {"years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds",
                                        "microseconds", "nanoseconds"};

struct DurationFormatObject : DOMObject {
    std::string locale, numbering_system, style;
    std::array<std::string, kUnits> unit_style, unit_display;
    std::optional<int> fractional_digits;
};

// A duration with each unit as a double (they are integers, validated).
using Duration = std::array<double, kUnits>;

bool is_numeric_like(const std::string& style) { return style == "numeric" || style == "2-digit"; }

// GetDurationUnitOptions
bool unit_options(Context& ctx, Object* options, size_t index, const std::string& base_style, const std::string& digital_base,
                  const std::string& previous_style, std::string& style_out, std::string& display_out) {
    const std::string unit = kUnitNames[index];
    std::optional<std::string> style;
    const bool is_time = index >= 4 && index <= 6;   // hours, minutes, seconds
    const bool is_sub_second = index >= 7;
    if (index <= 3) {
        if (!get_string_option(ctx, options, unit.c_str(), {"long", "short", "narrow"}, style)) return false;
    } else if (is_sub_second) {
        if (!get_string_option(ctx, options, unit.c_str(), {"long", "short", "narrow", "numeric"}, style)) return false;
    } else {
        if (!get_string_option(ctx, options, unit.c_str(), {"long", "short", "narrow", "numeric", "2-digit"}, style)) return false;
    }
    std::string display_default = "always";
    if (!style) {
        if (base_style == "digital") {
            if (!is_time) display_default = "auto";
            style = digital_base;
        } else if (previous_style == "fractional" || is_numeric_like(previous_style)) {
            if (index != 5 && index != 6) display_default = "auto";
            style = "numeric";
        } else {
            display_default = "auto";
            style = base_style;
        }
    }
    if (*style == "numeric" && is_sub_second) {
        style = "fractional";
        display_default = "auto";
    }
    std::optional<std::string> display;
    if (!get_string_option(ctx, options, (unit + "Display").c_str(), {"auto", "always"}, display)) return false;
    display_out = display ? *display : display_default;
    if (display_out == "always" && *style == "fractional") return range_error(ctx, unit + "Display can not be always with fractional style");
    if (previous_style == "fractional" && *style != "fractional") return range_error(ctx, "Invalid style for " + unit);
    if (is_numeric_like(previous_style) && *style != "fractional" && !is_numeric_like(*style)) {
        return range_error(ctx, "Invalid style for " + unit + " after a numeric unit");
    }
    if ((index == 5 || index == 6) && is_numeric_like(previous_style) && *style == "numeric") style = "2-digit";
    style_out = *style == "fractional" ? "numeric" : *style;
    return true;
}

Value construct_duration_format(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.DurationFormat requires 'new'"); return Value(); }
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::DurationFormat);
    if (ctx.has_exception()) return Value();
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
    Object* options = get_options_object(ctx, args.size() > 1 ? args[1] : Value());
    if (!options) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> numbering;
    if (!get_string_option(ctx, options, "numberingSystem", {}, numbering)) return Value();
    std::vector<std::pair<std::string, std::string>> overrides;
    if (numbering) {
        bool valid = !numbering->empty();
        size_t start = 0;
        while (valid) {
            size_t dash = numbering->find('-', start);
            size_t end = dash == std::string::npos ? numbering->size() : dash;
            if (end - start < 3 || end - start > 8) valid = false;
            for (size_t i = start; valid && i < end; i++) valid = std::isalnum(static_cast<unsigned char>((*numbering)[i])) != 0;
            if (dash == std::string::npos) break;
            start = dash + 1;
        }
        if (!valid) { ctx.throw_range_error("Invalid numberingSystem"); return Value(); }
        std::string lower = *numbering;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        overrides.emplace_back("nu", lower);
    }
    Resolved resolved = resolve_locale(requested, {{"nu", numbering_system_values}}, overrides, matcher);
    std::optional<std::string> style;
    if (!get_string_option(ctx, options, "style", {"long", "short", "narrow", "digital"}, style)) return Value();

    auto* object = Heap::Allocate<DurationFormatObject>();
    DurationFormatObject& f = *object;
    f.locale = resolved.locale;
    f.numbering_system = *resolved.value_of("nu");
    f.style = style ? *style : "short";
    std::string previous;
    for (size_t i = 0; i < kUnits; i++) {
        const char* digital_base = i <= 3 ? "short" : "numeric";
        if (!unit_options(ctx, options, i, f.style, digital_base, previous, f.unit_style[i], f.unit_display[i])) return Value();
        previous = (i >= 7 && f.unit_style[i] == "numeric") ? "fractional" : f.unit_style[i];
    }
    std::optional<double> fractional;
    if (!get_number_option(ctx, options, "fractionalDigits", 0, 9, std::nullopt, fractional)) return Value();
    if (fractional) f.fractional_digits = static_cast<int>(*fractional);
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

// ---- The duration argument -----------------------------------------------------------------------------------------

bool valid_duration(Context& ctx, const Duration& d) {
    int sign = 0;
    for (double v : d) {
        if (v == 0) continue;
        const int s = v < 0 ? -1 : 1;
        if (sign != 0 && s != sign) return range_error(ctx, "Invalid duration: mixed signs");
        sign = s;
    }
    const double limit32 = 4294967296.0;
    if (std::fabs(d[0]) >= limit32 || std::fabs(d[1]) >= limit32 || std::fabs(d[2]) >= limit32) {
        return range_error(ctx, "Invalid duration: out of range");
    }
    // days..nanoseconds as nanoseconds must stay below 2^53 seconds
    const __int128 ns = static_cast<__int128>(d[3]) * 86400000000000LL + static_cast<__int128>(d[4]) * 3600000000000LL +
                        static_cast<__int128>(d[5]) * 60000000000LL + static_cast<__int128>(d[6]) * 1000000000LL +
                        static_cast<__int128>(d[7]) * 1000000LL + static_cast<__int128>(d[8]) * 1000LL + static_cast<__int128>(d[9]);
    const __int128 bound = static_cast<__int128>(9007199254740992LL) * 1000000000LL;
    if (ns >= bound || ns <= -bound) return range_error(ctx, "Invalid duration: out of range");
    return true;
}

// ParseTemporalDurationString, for ISO 8601 durations such as "P1Y2M3W4DT5H6M7.5S".
bool parse_duration_string(const std::string& text, Duration& out) {
    out.fill(0);
    size_t i = 0;
    int sign = 1;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) { sign = text[i] == '-' ? -1 : 1; i++; }
    if (i >= text.size() || (text[i] != 'P' && text[i] != 'p')) return false;
    i++;
    bool in_time = false, any = false;
    int last_rank = -1;
    while (i < text.size()) {
        if (text[i] == 'T' || text[i] == 't') { if (in_time) return false; in_time = true; i++; continue; }
        const size_t start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == start) return false;
        std::string whole = text.substr(start, i - start), fraction;
        if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
            i++;
            const size_t f = i;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
            if (i == f || i - f > 9) return false;
            fraction = text.substr(f, i - f);
        }
        if (i >= text.size()) return false;
        const char designator = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i++])));
        int index;
        if (!in_time) index = designator == 'Y' ? 0 : designator == 'M' ? 1 : designator == 'W' ? 2 : designator == 'D' ? 3 : -1;
        else index = designator == 'H' ? 4 : designator == 'M' ? 5 : designator == 'S' ? 6 : -1;
        if (index < 0 || index <= last_rank) return false;
        last_rank = index;
        if (whole.size() > 16) return false;
        double value = std::stod(whole);
        if (!fraction.empty()) {
            if (index < 4) return false;
            // a fraction goes into the units below
            while (fraction.size() < 9) fraction += '0';
            long long nanos = std::stoll(fraction);
            if (index == 4) { out[5] = std::floor(nanos * 60.0 / 1e9); const long long rest = nanos * 60 % 1000000000; out[6] = std::floor(rest * 60.0 / 1e9); nanos = (rest * 60) % 1000000000; out[7] = std::floor(nanos / 1e6); out[8] = std::floor(std::fmod(nanos, 1e6) / 1e3); out[9] = std::fmod(nanos, 1e3); }
            else if (index == 5) { out[6] = std::floor(nanos * 60.0 / 1e9); nanos = nanos * 60 % 1000000000; out[7] = std::floor(nanos / 1e6); out[8] = std::floor(std::fmod(nanos, 1e6) / 1e3); out[9] = std::fmod(nanos, 1e3); }
            else { out[7] = std::floor(nanos / 1e6); out[8] = std::floor(std::fmod(nanos, 1e6) / 1e3); out[9] = std::fmod(nanos, 1e3); }
        }
        out[static_cast<size_t>(index)] = value;
        any = true;
    }
    if (!any) return false;
    for (double& v : out) v *= sign;
    return true;
}

// ToDurationRecord
bool to_duration(Context& ctx, const Value& input, Duration& out) {
    out.fill(0);
    if (input.is_string()) {
        if (!parse_duration_string(input.to_string(), out)) return range_error(ctx, "Invalid duration string");
        return valid_duration(ctx, out);
    }
    Object* object = input.is_function() ? static_cast<Object*>(input.as_function()) : input.as_object_or_null();
    if (!object) return type_error(ctx, "Duration must be an object or a string");
    // The properties are read in alphabetical order, as the Temporal duration record is.
    static const size_t order[kUnits] = {3, 4, 8, 7, 5, 1, 9, 6, 2, 0};
    bool any = false;
    for (size_t index : order) {
        Value value = object->get_property(kUnitNames[index]);
        if (ctx.has_exception()) return false;
        if (value.is_undefined()) continue;
        double number;
        if (!value.to_number_checked(ctx, number)) return false;
        if (!std::isfinite(number) || number != std::trunc(number)) return range_error(ctx, std::string(kUnitNames[index]) + " must be an integer");
        out[index] = number + 0.0;
        any = true;
    }
    if (!any) return type_error(ctx, "Duration has no fields");
    return valid_duration(ctx, out);
}

// ---- Formatting -------------------------------------------------------------------------------------------------------

// The seconds (or smaller) unit and everything below it as one decimal number: "6.007008009".
std::string fractional_value(const Duration& d, int exponent) {
    const __int128 sub = static_cast<__int128>(d[9]);
    __int128 ns = sub;
    switch (exponent) {
        case 9: ns += static_cast<__int128>(d[6]) * 1000000000LL; [[fallthrough]];
        case 6: ns += static_cast<__int128>(d[7]) * 1000000LL; [[fallthrough]];
        case 3: ns += static_cast<__int128>(d[8]) * 1000LL; break;
    }
    __int128 divisor = 1;
    for (int i = 0; i < exponent; i++) divisor *= 10;
    const bool negative = ns < 0;
    __int128 magnitude = negative ? -ns : ns;
    __int128 quotient = magnitude / divisor, remainder = magnitude % divisor;
    auto digits = [](__int128 v) {
        if (v == 0) return std::string("0");
        std::string s;
        while (v > 0) { s += static_cast<char>('0' + static_cast<int>(v % 10)); v /= 10; }
        return std::string(s.rbegin(), s.rend());
    };
    std::string fraction = digits(remainder);
    while (fraction.size() < static_cast<size_t>(exponent)) fraction = "0" + fraction;
    return std::string(negative ? "-" : "") + digits(quotient) + "." + fraction;
}

bool format_duration(Context& ctx, const DurationFormatObject& f, const Duration& d, std::vector<Part>& flattened) {
    std::vector<std::vector<Part>> result;
    bool need_separator = false;
    bool display_negative_sign = true;
    const bool negative_duration = std::any_of(d.begin(), d.end(), [](double v) { return v < 0; });
    const std::string locale = f.locale;

    for (size_t i = 0; i < kUnits; i++) {
        std::string value_text;
        double value = d[i];
        const std::string& style = f.unit_style[i];
        const std::string& display = f.unit_display[i];
        std::vector<std::pair<std::string, Value>> nf_options;
        bool done = false;
        bool combined = false;
        if (i == 6 || i == 7 || i == 8) {
            if (f.unit_style[i + 1] == "numeric") {
                value_text = fractional_value(d, i == 6 ? 9 : i == 7 ? 6 : 3);
                combined = true;
                const double digits = f.fractional_digits ? *f.fractional_digits : 9;
                nf_options.emplace_back("maximumFractionDigits", Value(digits));
                nf_options.emplace_back("minimumFractionDigits", Value(static_cast<double>(f.fractional_digits.value_or(0))));
                nf_options.emplace_back("roundingMode", Value("trunc"));
                done = true;
            }
        }
        bool display_required = false;
        if (i == 5 && need_separator) {
            display_required = f.unit_display[6] == "always" || d[6] != 0 || d[7] != 0 || d[8] != 0 || d[9] != 0;
        }
        const bool is_zero = combined ? value_text.find_first_not_of("0.-") == std::string::npos : value == 0;
        if (!is_zero || display != "auto" || display_required) {
            Numeric number;
            if (combined) {
                auto parsed = icu4x::Decimal::from_string(value_text);
                number.value = std::move(parsed).ok().value();
            } else {
                number = numeric_from_double(value);
            }
            if (display_negative_sign) {
                display_negative_sign = false;
                if (is_zero && negative_duration) {
                    number.value = std::move(icu4x::Decimal::from_string("-0")).ok().value();
                }
            } else {
                nf_options.emplace_back("signDisplay", Value("never"));
            }
            nf_options.emplace_back("numberingSystem", Value(f.numbering_system));
            if (style == "2-digit") nf_options.emplace_back("minimumIntegerDigits", Value(2.0));
            if (style != "numeric" && style != "2-digit") {
                static const char* const singular[kUnits] = {"year", "month", "week", "day", "hour", "minute", "second", "millisecond", "microsecond", "nanosecond"};
                nf_options.emplace_back("style", Value("unit"));
                nf_options.emplace_back("unit", Value(singular[i]));
                nf_options.emplace_back("unitDisplay", Value(style));
            } else {
                nf_options.emplace_back("useGrouping", Value(false));
            }
            std::vector<Part> parts;
            if (!format_number_parts(ctx, locale, nf_options, number, parts)) return false;
            static const char* const singular_names[kUnits] = {"year", "month", "week", "day", "hour", "minute", "second", "millisecond", "microsecond", "nanosecond"};
            for (Part& p : parts) p.unit = singular_names[i];
            std::vector<Part>* list;
            if (!need_separator) {
                result.emplace_back();
                list = &result.back();
            } else {
                list = &result.back();
                list->push_back({"literal", ":", ""});
            }
            for (Part& p : parts) list->push_back(std::move(p));
            if (!need_separator && (style == "2-digit" || style == "numeric")) need_separator = true;
        }
        if (done) break;
    }

    std::string list_style = f.style == "digital" ? "short" : f.style;
    std::vector<std::string> strings;
    for (const auto& parts : result) {
        std::string text;
        for (const Part& p : parts) text += p.value;
        strings.push_back(std::move(text));
    }
    std::vector<std::pair<bool, std::string>> listed;
    if (!list_format_parts(ctx, locale, "unit", list_style, strings, listed)) return false;
    size_t next = 0;
    for (auto& [is_element, text] : listed) {
        if (is_element) {
            for (Part& p : result[next]) flattened.push_back(std::move(p));
            next++;
        } else {
            flattened.push_back({"literal", text, ""});
        }
    }
    return true;
}

}  // namespace

void register_duration_format(Context& ctx, Object* intl) {
    Class duration = define_class(ctx, intl, "DurationFormat", 0, Kind::DurationFormat, construct_duration_format);
    add_supported_locales_of(ctx, duration.constructor);
    Object* proto = duration.prototype;

    define_method(proto, "format", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DurationFormatObject>(ctx, receiver, "DurationFormat");
        if (!self) return Value();
        Duration d;
        if (!to_duration(ctx, args.empty() ? Value() : args[0], d)) return Value();
        std::vector<Part> parts;
        if (!format_duration(ctx, *self, d, parts)) return Value();
        std::string text;
        for (const Part& p : parts) text += p.value;
        return Value(text);
    });

    define_method(proto, "formatToParts", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DurationFormatObject>(ctx, receiver, "DurationFormat");
        if (!self) return Value();
        Duration d;
        if (!to_duration(ctx, args.empty() ? Value() : args[0], d)) return Value();
        std::vector<Part> parts;
        if (!format_duration(ctx, *self, d, parts)) return Value();
        auto array = ObjectFactory::create_array(0);
        uint32_t index = 0;
        for (const Part& p : parts) {
            Object* entry = make_object({{"type", Value(p.type)}, {"value", Value(p.value)}});
            if (!p.unit.empty()) put(entry, "unit", Value(p.unit));
            array->set_element(index++, Value(entry));
        }
        return Value(array.release());
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<DurationFormatObject>(ctx, receiver, "DurationFormat");
        if (!self) return Value();
        auto result = ObjectFactory::create_object();
        put(result.get(), "locale", Value(self->locale));
        put(result.get(), "numberingSystem", Value(self->numbering_system));
        put(result.get(), "style", Value(self->style));
        for (size_t i = 0; i < kUnits; i++) {
            put(result.get(), kUnitNames[i], Value(self->unit_style[i]));
            put(result.get(), (std::string(kUnitNames[i]) + "Display").c_str(), Value(self->unit_display[i]));
        }
        if (self->fractional_digits) put(result.get(), "fractionalDigits", Value(static_cast<double>(*self->fractional_digits)));
        return Value(result.release());
    });
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
