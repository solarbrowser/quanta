/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include "icu4x/Locale.hpp"

#include <cmath>
#include "icu4x/PluralCategories.hpp"
#include "icu4x/PluralCategory.hpp"
#include "icu4x/PluralOperands.hpp"
#include "icu4x/PluralRules.hpp"
#include "icu4x/PluralRulesWithRanges.hpp"

namespace Quanta::IntlImpl {

namespace {

struct PluralRulesObject : DOMObject {
    std::string locale;
    bool ordinal = false;
    std::string notation = "standard";
    std::string compact_display = "short";
    DigitOptions digits;
    std::unique_ptr<icu4x::PluralRules> rules;
    std::unique_ptr<icu4x::PluralRulesWithRanges> ranges;   // made by the first selectRange
};

const char* category_name(icu4x::PluralCategory category) {
    using C = icu4x::PluralCategory;
    if (category == C::Zero) return "zero";
    if (category == C::One) return "one";
    if (category == C::Two) return "two";
    if (category == C::Few) return "few";
    if (category == C::Many) return "many";
    return "other";
}

std::unique_ptr<icu4x::PluralOperands> operands_of(const PluralRulesObject& self, const Numeric& number) {
    auto rounded = icu4x::Decimal::from_string(number.value->to_string());
    std::unique_ptr<icu4x::Decimal> value = std::move(rounded).ok().value();
    // The exponent a notation scales by: scientific takes one digit before the point, engineering a multiple of three,
    // compact the thousands steps (CLDR's compact patterns start at 1000; languages with other steps are not told apart).
    int exponent = 0;
    if (self.notation != "standard" && !value->is_zero()) {
        const int magnitude = value->nonzero_magnitude_start();
        if (self.notation == "scientific") exponent = magnitude;
        else if (self.notation == "engineering") exponent = static_cast<int>(std::floor(magnitude / 3.0)) * 3;
        else if (magnitude >= 3) exponent = static_cast<int>(std::floor(magnitude / 3.0)) * 3;
    }
    value->multiply_pow10(static_cast<int16_t>(-exponent));
    apply_digit_options(self.digits, *value);
    if (exponent != 0) return icu4x::PluralOperands::from_significand_and_exponent(*value, static_cast<uint8_t>(std::abs(exponent)));
    return icu4x::PluralOperands::from_fixed_decimal(*value);
}

Value construct_plural_rules(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.PluralRules requires 'new'"); return Value(); }
    std::vector<std::string> requested;
    Object* options;
    if (!read_locales_and_options(ctx, args, requested, options)) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> type;
    if (!get_string_option(ctx, options, "type", {"cardinal", "ordinal"}, type)) return Value();
    std::optional<std::string> notation, compact_display;
    if (!get_string_option(ctx, options, "notation", {"standard", "scientific", "engineering", "compact"}, notation)) return Value();
    if (!get_string_option(ctx, options, "compactDisplay", {"short", "long"}, compact_display)) return Value();
    DigitOptions digits;
    if (!read_digit_options(ctx, options, 0, 3, notation && *notation == "compact", digits)) return Value();
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::PluralRules);
    if (ctx.has_exception()) return Value();

    Resolved resolved = resolve_locale(requested, {}, {}, matcher);
    auto* object = Heap::Allocate<PluralRulesObject>();
    object->locale = resolved.locale;
    object->ordinal = type && *type == "ordinal";
    object->digits = digits;
    if (notation) object->notation = *notation;
    if (compact_display) object->compact_display = *compact_display;
    std::unique_ptr<icu4x::Locale> icu_locale = IntlICU::to_icu(resolved.data_locale);
    if (!icu_locale) { ctx.throw_range_error("Incorrect locale information provided"); return Value(); }
    auto rules = object->ordinal
        ? icu4x::PluralRules::create_ordinal_with_provider(IntlICU::provider(), *icu_locale)
        : icu4x::PluralRules::create_cardinal_with_provider(IntlICU::provider(), *icu_locale);
    if (!rules.is_ok()) { ctx.throw_range_error("Plural rules are not available for this locale"); return Value(); }
    object->rules = std::move(rules).ok().value();
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

}  // namespace

void register_plural_rules(Context& ctx, Object* intl) {
    Class plural = define_class(ctx, intl, "PluralRules", 0, Kind::PluralRules, construct_plural_rules);
    add_supported_locales_of(ctx, plural.constructor);
    Object* proto = plural.prototype;

    define_method(proto, "select", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<PluralRulesObject>(ctx, receiver, "PluralRules");
        if (!self) return Value();
        double number;
        if (!(args.empty() ? Value() : args[0]).to_number_checked(ctx, number)) return Value();
        if (!std::isfinite(number)) return Value("other");
        Numeric numeric = numeric_from_double(number);
        return Value(category_name(self->rules->category_for(*operands_of(*self, numeric))));
    });

    define_method(proto, "selectRange", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<PluralRulesObject>(ctx, receiver, "PluralRules");
        if (!self) return Value();
        const Value start = args.size() > 0 ? args[0] : Value();
        const Value end = args.size() > 1 ? args[1] : Value();
        if (start.is_undefined() || end.is_undefined()) { ctx.throw_type_error("start and end must be provided"); return Value(); }
        double x, y;
        if (!start.to_number_checked(ctx, x) || !end.to_number_checked(ctx, y)) return Value();
        if (std::isnan(x) || std::isnan(y)) { ctx.throw_range_error("start and end must not be NaN"); return Value(); }
        if (!std::isfinite(x) || !std::isfinite(y)) return Value("other");
        if (!self->ranges) {
            std::unique_ptr<icu4x::Locale> icu_locale = IntlICU::to_icu(IntlICU::best_available(self->locale));
            auto ranges = self->ordinal
                ? icu4x::PluralRulesWithRanges::create_ordinal_with_provider(IntlICU::provider(), *icu_locale)
                : icu4x::PluralRulesWithRanges::create_cardinal_with_provider(IntlICU::provider(), *icu_locale);
            if (!ranges.is_ok()) return Value("other");
            self->ranges = std::move(ranges).ok().value();
        }
        Numeric a = numeric_from_double(x), b = numeric_from_double(y);
        return Value(category_name(self->ranges->category_for_range(*operands_of(*self, a), *operands_of(*self, b))));
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<PluralRulesObject>(ctx, receiver, "PluralRules");
        if (!self) return Value();
        const DigitOptions& d = self->digits;
        auto result = ObjectFactory::create_object();
        auto number = [](int n) { return Value(static_cast<double>(n)); };
        put(result.get(), "locale", Value(self->locale));
        put(result.get(), "type", Value(self->ordinal ? "ordinal" : "cardinal"));
        put(result.get(), "notation", Value(self->notation));
        if (self->notation == "compact") put(result.get(), "compactDisplay", Value(self->compact_display));
        put(result.get(), "minimumIntegerDigits", number(d.minimum_integer_digits));
        if (d.has_fraction_slots()) {
            put(result.get(), "minimumFractionDigits", number(d.minimum_fraction_digits));
            put(result.get(), "maximumFractionDigits", number(d.maximum_fraction_digits));
        }
        if (d.has_significant_slots()) {
            put(result.get(), "minimumSignificantDigits", number(d.minimum_significant_digits));
            put(result.get(), "maximumSignificantDigits", number(d.maximum_significant_digits));
        }
        icu4x::PluralCategories categories = self->rules->categories();
        std::vector<std::string> names;
        if (categories.zero) names.push_back("zero");
        if (categories.one) names.push_back("one");
        if (categories.two) names.push_back("two");
        if (categories.few) names.push_back("few");
        if (categories.many) names.push_back("many");
        if (categories.other) names.push_back("other");
        put(result.get(), "pluralCategories", string_array(names));
        put(result.get(), "roundingIncrement", number(d.rounding_increment));
        put(result.get(), "roundingMode", Value(d.rounding_mode));
        put(result.get(), "roundingPriority", Value(d.rounding_priority));
        put(result.get(), "trailingZeroDisplay", Value(d.trailing_zero_display));
        return Value(result.release());
    });
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
