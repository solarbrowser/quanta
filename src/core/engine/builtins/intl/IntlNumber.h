/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

// The digit options and the number-to-decimal step that NumberFormat, PluralRules and the other number-formatting
// services share. Compiled only with ICU4X.
#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "icu4x/Decimal.hpp"
#include "icu4x/DecimalFormatter.hpp"
#include "icu4x/DecimalGroupingStrategy.hpp"

#include <memory>
#include <optional>
#include <string>

namespace Quanta::IntlImpl {

enum class RoundingType : uint8_t { FractionDigits, SignificantDigits, MorePrecision, LessPrecision };

// What SetNumberFormatDigitOptions leaves in the internal slots of a service.
struct DigitOptions {
    int minimum_integer_digits = 1;
    int minimum_fraction_digits = 0, maximum_fraction_digits = 3;
    int minimum_significant_digits = 1, maximum_significant_digits = 21;
    RoundingType type = RoundingType::FractionDigits;
    int rounding_increment = 1;
    std::string rounding_mode = "halfExpand";
    std::string rounding_priority = "auto";
    std::string trailing_zero_display = "auto";

    bool has_fraction_slots() const { return type != RoundingType::SignificantDigits; }
    bool has_significant_slots() const { return type != RoundingType::FractionDigits; }
};

// SetNumberFormatDigitOptions. False with an exception pending.
bool read_digit_options(Context& ctx, Object* options, int default_minimum_fraction, int default_maximum_fraction,
                        bool compact_notation, DigitOptions& out);

// A mathematical value, or one of the numbers a decimal cannot hold.
struct Numeric {
    enum class Kind : uint8_t { Finite, NaN, PositiveInfinity, NegativeInfinity } kind = Kind::Finite;
    std::unique_ptr<icu4x::Decimal> value;   // set when finite; carries the sign, so it can be a negative zero
    bool negative() const;
};

// ToIntlMathematicalValue. False with an exception pending.
bool to_intl_numeric(Context& ctx, const Value& value, Numeric& out);
// A Number as a Numeric.
Numeric numeric_from_double(double number);

// What the locale writes numbers with: the ten digits and the separators, learned by formatting probes, which is how
// the parts of a formatted number are told apart.
struct Symbols {
    std::string digit[10];
    std::string decimal, group, minus, plus;
};

struct Part {
    std::string type, value;
    std::string unit;     // RelativeTimeFormat's number parts name the unit they count
    std::string source;   // formatRangeToParts: "startRange", "endRange" or "shared"
};

std::unique_ptr<icu4x::Decimal> copy_of(const icu4x::Decimal& d);
// A decimal formatter for `icu_locale` (the -u-nu- keyword in it picks the digits).
std::unique_ptr<icu4x::DecimalFormatter> make_formatter(const std::string& icu_locale, icu4x::DecimalGroupingStrategy strategy);
// The values of the "nu" key for a locale: the numbering system its data writes with, then the other numeric ones.
std::vector<std::string> numbering_system_values(const std::string& data_locale);
Symbols learn_symbols(const std::string& icu_locale);
// The number `text` taken apart into integer, group, decimal and fraction parts.
void number_parts(const Symbols& symbols, const std::string& text, std::vector<Part>& out);

// A number written the way new Intl.NumberFormat(locale, options).formatToParts(value) would, for the services built on it.
// `options` are the (name, value) pairs of the options object. False with an exception pending.
bool format_number_parts(Context& ctx, const std::string& locale, const std::vector<std::pair<std::string, Value>>& options,
                         const Numeric& value, std::vector<Part>& out);
// new Intl.ListFormat(locale, { type, style }).formatToParts(items): (is_element, text) for each part.
bool list_format_parts(Context& ctx, const std::string& locale, const char* type, const std::string& style,
                       const std::vector<std::string>& items, std::vector<std::pair<bool, std::string>>& out);

// Applies the rounding, padding and trailing-zero rules of `digits` to a finite value.
void apply_digit_options(const DigitOptions& digits, icu4x::Decimal& value);

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
