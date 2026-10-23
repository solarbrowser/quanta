/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include "quanta/core/runtime/BigInt.h"

#include "icu4x/DecimalRoundingIncrement.hpp"
#include "icu4x/DecimalSignDisplay.hpp"
#include "icu4x/Locale.hpp"
#include "icu4x/DecimalSign.hpp"
#include "icu4x/DecimalSignedRoundingMode.hpp"

#include <algorithm>
#include <cmath>

namespace Quanta::IntlImpl {

bool read_digit_options(Context& ctx, Object* options, int default_minimum_fraction, int default_maximum_fraction,
                        bool compact_notation, DigitOptions& out) {
    std::optional<double> minimum_integer;
    if (!get_number_option(ctx, options, "minimumIntegerDigits", 1, 21, 1, minimum_integer)) return false;
    Value minimum_fraction_value = options->get_property("minimumFractionDigits");
    if (ctx.has_exception()) return false;
    Value maximum_fraction_value = options->get_property("maximumFractionDigits");
    if (ctx.has_exception()) return false;
    Value minimum_significant_value = options->get_property("minimumSignificantDigits");
    if (ctx.has_exception()) return false;
    Value maximum_significant_value = options->get_property("maximumSignificantDigits");
    if (ctx.has_exception()) return false;
    out.minimum_integer_digits = static_cast<int>(*minimum_integer);

    std::optional<double> increment;
    if (!get_number_option(ctx, options, "roundingIncrement", 1, 5000, 1, increment)) return false;
    static const int allowed[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
    if (std::find(std::begin(allowed), std::end(allowed), static_cast<int>(*increment)) == std::end(allowed)) {
        return range_error(ctx, "Value out of range for Intl option roundingIncrement");
    }
    std::optional<std::string> mode, priority, trailing;
    if (!get_string_option(ctx, options, "roundingMode",
                           {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven"}, mode)) return false;
    if (!get_string_option(ctx, options, "roundingPriority", {"auto", "morePrecision", "lessPrecision"}, priority)) return false;
    if (!get_string_option(ctx, options, "trailingZeroDisplay", {"auto", "stripIfInteger"}, trailing)) return false;
    out.rounding_increment = static_cast<int>(*increment);
    out.rounding_mode = mode ? *mode : "halfExpand";
    out.rounding_priority = priority ? *priority : "auto";
    out.trailing_zero_display = trailing ? *trailing : "auto";
    if (out.rounding_increment != 1) default_maximum_fraction = default_minimum_fraction;

    const bool has_significant = !minimum_significant_value.is_undefined() || !maximum_significant_value.is_undefined();
    const bool has_fraction = !minimum_fraction_value.is_undefined() || !maximum_fraction_value.is_undefined();
    bool need_significant = true, need_fraction = true;
    if (out.rounding_priority == "auto") {
        need_significant = has_significant;
        if (need_significant || (!has_fraction && compact_notation)) need_fraction = false;
    }
    if (need_significant) {
        if (has_significant) {
            std::optional<double> minimum, maximum;
            if (!default_number_option(ctx, minimum_significant_value, 1, 21, 1, minimum, "minimumSignificantDigits")) return false;
            if (!default_number_option(ctx, maximum_significant_value, *minimum, 21, 21, maximum, "maximumSignificantDigits")) return false;
            out.minimum_significant_digits = static_cast<int>(*minimum);
            out.maximum_significant_digits = static_cast<int>(*maximum);
        } else {
            out.minimum_significant_digits = 1;
            out.maximum_significant_digits = 21;
        }
    }
    if (need_fraction) {
        if (has_fraction) {
            std::optional<double> minimum, maximum;
            if (!default_number_option(ctx, minimum_fraction_value, 0, 100, std::nullopt, minimum, "minimumFractionDigits")) return false;
            if (!default_number_option(ctx, maximum_fraction_value, 0, 100, std::nullopt, maximum, "maximumFractionDigits")) return false;
            if (!minimum) minimum = std::min<double>(default_minimum_fraction, *maximum);
            else if (!maximum) maximum = std::max<double>(default_maximum_fraction, *minimum);
            else if (*minimum > *maximum) return range_error(ctx, "maximumFractionDigits value is out of range.");
            out.minimum_fraction_digits = static_cast<int>(*minimum);
            out.maximum_fraction_digits = static_cast<int>(*maximum);
        } else {
            out.minimum_fraction_digits = default_minimum_fraction;
            out.maximum_fraction_digits = default_maximum_fraction;
        }
    }
    if (!need_significant && !need_fraction) {
        out.minimum_fraction_digits = 0;
        out.maximum_fraction_digits = 0;
        out.minimum_significant_digits = 1;
        out.maximum_significant_digits = 2;
        out.type = RoundingType::MorePrecision;
        out.rounding_priority = "morePrecision";
    } else if (out.rounding_priority == "morePrecision") {
        out.type = RoundingType::MorePrecision;
    } else if (out.rounding_priority == "lessPrecision") {
        out.type = RoundingType::LessPrecision;
    } else if (has_significant) {
        out.type = RoundingType::SignificantDigits;
    } else {
        out.type = RoundingType::FractionDigits;
    }
    if (out.rounding_increment != 1) {
        if (out.type != RoundingType::FractionDigits) {
            return type_error(ctx, "roundingIncrement requires fraction digit rounding");
        }
        if (out.maximum_fraction_digits != out.minimum_fraction_digits) {
            return range_error(ctx, "maximumFractionDigits and minimumFractionDigits must be equal with roundingIncrement");
        }
    }
    return true;
}

std::unique_ptr<icu4x::Decimal> copy_of(const icu4x::Decimal& d) {
    return std::move(icu4x::Decimal::from_string(d.to_string())).ok().value();
}

std::unique_ptr<icu4x::DecimalFormatter> make_formatter(const std::string& icu_locale, icu4x::DecimalGroupingStrategy strategy) {
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(icu_locale);
    if (!locale) return nullptr;
    auto made = icu4x::DecimalFormatter::create_with_grouping_strategy_and_provider(IntlICU::provider(), *locale, strategy);
    return made.is_ok() ? std::move(made).ok().value() : nullptr;
}

std::vector<std::string> numbering_system_values(const std::string& data_locale) {
    // The locale's own system first (the default), then every numeric system the data has digits for.
    std::vector<std::string> values;
    if (std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(data_locale)) {
        auto made = icu4x::DecimalFormatter::create_with_grouping_strategy_and_provider(IntlICU::provider(), *locale, icu4x::DecimalGroupingStrategy::Never);
        if (made.is_ok()) {
            const std::string zero = std::move(made).ok().value()->format(*icu4x::Decimal::from(0));
            const std::string_view system = numbering_system_of_zero(zero);
            if (!system.empty()) values.emplace_back(system);
        }
    }
    if (values.empty()) values.push_back("latn");
    for (std::string& name : supported_values("numberingSystem")) {
        if (name != values[0]) values.push_back(std::move(name));
    }
    return values;
}

std::string without(std::string text, const std::string& piece) {
    if (const size_t at = text.find(piece); at != std::string::npos && !piece.empty()) text.erase(at, piece.size());
    return text;
}

Symbols learn_symbols(const std::string& icu_locale) {
    Symbols s;
    auto plain = make_formatter(icu_locale, icu4x::DecimalGroupingStrategy::Never);
    auto grouped = make_formatter(icu_locale, icu4x::DecimalGroupingStrategy::Always);
    if (!plain || !grouped) return s;
    for (int d = 0; d < 10; d++) s.digit[d] = plain->format(*icu4x::Decimal::from(d));
    {   // 1.5 -> digit(1) decimal digit(5)
        auto value = icu4x::Decimal::from_string("1.5");
        std::string text = plain->format(*std::move(value).ok().value());
        s.decimal = without(without(text, s.digit[1]), s.digit[5]);
    }
    {   // 1000000 with grouping: digit(1) group digit(0) ...
        std::string text = grouped->format(*icu4x::Decimal::from(1000000));
        const size_t one = text.find(s.digit[1]);
        const size_t zero = text.find(s.digit[0], one == std::string::npos ? 0 : one + s.digit[1].size());
        if (one != std::string::npos && zero != std::string::npos) {
            s.group = text.substr(one + s.digit[1].size(), zero - one - s.digit[1].size());
        }
    }
    {
        auto negative = icu4x::Decimal::from(-1);
        s.minus = without(plain->format(*negative), s.digit[1]);
        auto positive = icu4x::Decimal::from(1);
        positive->apply_sign_display(icu4x::DecimalSignDisplay::Always);
        s.plus = without(plain->format(*positive), s.digit[1]);
    }
    return s;
}

// The number `text` taken apart into integer, group, decimal and fraction parts.
void number_parts(const Symbols& s, const std::string& text, std::vector<Part>& out) {
    size_t i = 0;
    bool fraction = false;
    auto starts = [&](const std::string& piece) { return !piece.empty() && text.compare(i, piece.size(), piece) == 0; };
    while (i < text.size()) {
        bool matched = false;
        for (const std::string& digit : s.digit) {
            if (starts(digit)) {
                const char* type = fraction ? "fraction" : "integer";
                if (!out.empty() && out.back().type == type) out.back().value += digit;
                else out.push_back({type, digit});
                i += digit.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (!fraction && starts(s.decimal) && i > 0) {
            out.push_back({"decimal", s.decimal});
            i += s.decimal.size();
            fraction = true;
        } else if (!fraction && starts(s.group)) {
            out.push_back({"group", s.group});
            i += s.group.size();
        } else {
            // Something the probes did not meet (a numeral system's own punctuation): kept as it is.
            out.push_back({"literal", std::string(1, text[i])});
            i++;
        }
    }
}

bool Numeric::negative() const {
    switch (kind) {
        case Kind::NegativeInfinity: return true;
        case Kind::Finite: return value->sign() == icu4x::DecimalSign::Negative;
        default: return false;
    }
}

Numeric numeric_from_double(double number) {
    Numeric result;
    if (std::isnan(number)) { result.kind = Numeric::Kind::NaN; return result; }
    if (std::isinf(number)) {
        result.kind = number < 0 ? Numeric::Kind::NegativeInfinity : Numeric::Kind::PositiveInfinity;
        return result;
    }
    auto converted = icu4x::Decimal::from_double_with_round_trip_precision(std::fabs(number));
    result.value = std::move(converted).ok().value();
    if (std::signbit(number)) result.value->set_sign(icu4x::DecimalSign::Negative);
    return result;
}

namespace {

bool plain_decimal_text(const std::string& s) {
    // Only what a decimal parser takes whole: [+-]digits[.digits][e[+-]digits]; "Infinity" and the radix forms go
    // through the Number conversion instead.
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    }
    if (digits == 0) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        size_t exponent = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { i++; exponent++; }
        if (exponent == 0) return false;
    }
    return i == s.size();
}

std::string trim_whitespace(const std::string& s) {
    static const char* const space = " \t\n\v\f\r";
    size_t begin = s.find_first_not_of(space);
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(space) - begin + 1);
}

}  // namespace

bool to_intl_numeric(Context& ctx, const Value& input, Numeric& out) {
    Value primitive = input;
    if (input.is_object() || input.is_function()) {
        Object* object = input.is_function() ? static_cast<Object*>(input.as_function()) : input.as_object();
        primitive = object->to_primitive(ctx, "number");
        if (ctx.has_exception()) return false;
    }
    if (primitive.is_bigint()) {
        auto parsed = icu4x::Decimal::from_string(primitive.as_bigint()->to_string());
        if (!parsed.is_ok()) return type_error(ctx, "Invalid BigInt");
        out.kind = Numeric::Kind::Finite;
        out.value = std::move(parsed).ok().value();
        return true;
    }
    if (primitive.is_string()) {
        std::string text = trim_whitespace(primitive.to_string());
        if (plain_decimal_text(text)) {
            auto parsed = icu4x::Decimal::from_string(text[0] == '+' ? text.substr(1) : text);
            if (parsed.is_ok()) {
                out.kind = Numeric::Kind::Finite;
                out.value = std::move(parsed).ok().value();
                return true;
            }
        }
    }
    double number;
    if (!primitive.to_number_checked(ctx, number)) return false;
    out = numeric_from_double(number);
    return true;
}

void apply_digit_options(const DigitOptions& d, icu4x::Decimal& value) {
    using Mode = icu4x::DecimalSignedRoundingMode;
    static const std::pair<const char*, Mode::Value> modes[] = {
        {"ceil", Mode::Ceil}, {"floor", Mode::Floor}, {"expand", Mode::Expand}, {"trunc", Mode::Trunc},
        {"halfCeil", Mode::HalfCeil}, {"halfFloor", Mode::HalfFloor}, {"halfExpand", Mode::HalfExpand},
        {"halfTrunc", Mode::HalfTrunc}, {"halfEven", Mode::HalfEven},
    };
    Mode mode = Mode::HalfExpand;
    for (const auto& [name, candidate] : modes) {
        if (d.rounding_mode == name) mode = candidate;
    }

    // The increment as a multiple of one of ICU4X's (1, 2, 5, 25) at a position shifted up by `shift`.
    struct Step { int increment; icu4x::DecimalRoundingIncrement::Value base; int shift; };
    static const Step steps[] = {
        {1, icu4x::DecimalRoundingIncrement::MultiplesOf1, 0}, {2, icu4x::DecimalRoundingIncrement::MultiplesOf2, 0},
        {5, icu4x::DecimalRoundingIncrement::MultiplesOf5, 0}, {10, icu4x::DecimalRoundingIncrement::MultiplesOf1, 1},
        {20, icu4x::DecimalRoundingIncrement::MultiplesOf2, 1}, {25, icu4x::DecimalRoundingIncrement::MultiplesOf25, 0},
        {50, icu4x::DecimalRoundingIncrement::MultiplesOf5, 1}, {100, icu4x::DecimalRoundingIncrement::MultiplesOf1, 2},
        {200, icu4x::DecimalRoundingIncrement::MultiplesOf2, 2}, {250, icu4x::DecimalRoundingIncrement::MultiplesOf25, 1},
        {500, icu4x::DecimalRoundingIncrement::MultiplesOf5, 2}, {1000, icu4x::DecimalRoundingIncrement::MultiplesOf1, 3},
        {2000, icu4x::DecimalRoundingIncrement::MultiplesOf2, 3}, {2500, icu4x::DecimalRoundingIncrement::MultiplesOf25, 2},
        {5000, icu4x::DecimalRoundingIncrement::MultiplesOf5, 3},
    };
    Step step = steps[0];
    for (const Step& s : steps) {
        if (s.increment == d.rounding_increment) step = s;
    }

    const int magnitude = value.is_zero() ? 0 : value.nonzero_magnitude_start();
    const int fraction_position = -d.maximum_fraction_digits;
    const int significant_position = magnitude - d.maximum_significant_digits + 1;

    bool use_significant = d.type == RoundingType::SignificantDigits;
    if (d.type == RoundingType::MorePrecision) use_significant = significant_position <= fraction_position;
    else if (d.type == RoundingType::LessPrecision) use_significant = significant_position > fraction_position;

    if (use_significant) {
        value.round_with_mode(static_cast<int16_t>(significant_position), mode);
        value.trim_end();
        const int rounded = value.is_zero() ? 0 : value.nonzero_magnitude_start();
        value.pad_end(static_cast<int16_t>(rounded - d.minimum_significant_digits + 1));
    } else {
        value.round_with_mode_and_increment(static_cast<int16_t>(fraction_position + step.shift), mode,
                                            icu4x::DecimalRoundingIncrement(step.base));
        value.trim_end();
        value.pad_end(static_cast<int16_t>(-d.minimum_fraction_digits));
    }
    if (d.trailing_zero_display == "stripIfInteger") value.trim_end_if_integer();
    if (!value.is_zero()) value.trim_start();   // a scaling by a power of ten leaves the zeros it moved past
    value.pad_start(static_cast<int16_t>(d.minimum_integer_digits));
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
