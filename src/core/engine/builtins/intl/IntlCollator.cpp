/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "icu4x/Collator.hpp"
#include "icu4x/CollatorAlternateHandling.hpp"
#include "icu4x/CollatorCaseLevel.hpp"
#include "icu4x/CollatorOptionsV1.hpp"
#include "icu4x/CollatorStrength.hpp"
#include "icu4x/Locale.hpp"

#include <algorithm>

namespace Quanta::IntlImpl {

namespace {

struct CollatorObject : DOMObject {
    std::string locale, usage, sensitivity, collation, case_first;
    bool ignore_punctuation = false, numeric = false;
    std::unique_ptr<icu4x::Collator> collator;
    TracedValue bound_compare;
    void Visit(Visitor&) {}
};

// Collation types a locale lists, without the two the specification keeps out of the "co" values.
std::vector<std::string> collation_values(const std::string& data_locale) {
    std::vector<std::string> values = {"default"};
    for (std::string& type : collation_types(data_locale)) {
        if (type != "standard" && type != "search") values.push_back(std::move(type));
    }
    return values;
}

std::vector<std::string> numeric_values(const std::string&) { return {"false", "true"}; }
std::vector<std::string> case_first_values(const std::string&) { return {"false", "lower", "upper"}; }

bool is_unicode_type(std::string_view s) {
    size_t start = 0;
    if (s.empty()) return false;
    while (true) {
        size_t dash = s.find('-', start);
        size_t end = dash == std::string_view::npos ? s.size() : dash;
        if (end - start < 3 || end - start > 8) return false;
        for (size_t i = start; i < end; i++) {
            if (!std::isalnum(static_cast<unsigned char>(s[i]))) return false;
        }
        if (dash == std::string_view::npos) return true;
        start = dash + 1;
    }
}

Value construct_collator(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    std::vector<std::string> requested;
    Object* options;
    if (!read_locales_and_options(ctx, args, requested, options)) return Value();
    std::optional<std::string> usage;
    if (!get_string_option(ctx, options, "usage", {"sort", "search"}, usage)) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> collation;
    if (!get_string_option(ctx, options, "collation", {}, collation)) return Value();
    if (collation && !is_unicode_type(*collation)) return range_error(ctx, "Invalid collation"), Value();
    std::optional<bool> numeric;
    if (!get_boolean_option(ctx, options, "numeric", numeric)) return Value();
    std::optional<std::string> case_first;
    if (!get_string_option(ctx, options, "caseFirst", {"upper", "lower", "false"}, case_first)) return Value();

    std::vector<std::pair<std::string, std::string>> overrides;
    if (collation) overrides.emplace_back("co", *collation);
    if (numeric) overrides.emplace_back("kn", *numeric ? "true" : "false");
    if (case_first) overrides.emplace_back("kf", *case_first);
    Resolved resolved = resolve_locale(requested, {{"co", collation_values}, {"kn", numeric_values}, {"kf", case_first_values}},
                                       overrides, matcher);

    std::optional<std::string> sensitivity;
    if (!get_string_option(ctx, options, "sensitivity", {"base", "accent", "case", "variant"}, sensitivity)) return Value();
    std::optional<bool> ignore_punctuation;
    if (!get_boolean_option(ctx, options, "ignorePunctuation", ignore_punctuation)) return Value();
    Object* proto = prototype_from_new_target(ctx, is_construct ? new_target : Value(), Kind::Collator);
    if (ctx.has_exception()) return Value();

    auto* object = Heap::Allocate<CollatorObject>();
    object->locale = resolved.locale;
    object->usage = usage ? *usage : "sort";
    object->sensitivity = sensitivity ? *sensitivity : "variant";
    object->collation = *resolved.value_of("co");
    object->numeric = *resolved.value_of("kn") == "true";
    object->case_first = *resolved.value_of("kf");
    // Thai (and Lao) ignore punctuation unless told otherwise.
    const std::string language = IntlICU::split_tag(resolved.data_locale).language;
    object->ignore_punctuation = ignore_punctuation ? *ignore_punctuation : (language == "th" || language == "lo");

    // ICU4X reads the type, numeric ordering and case order from the locale's own keywords.
    LocaleId id = IntlICU::split_tag(resolved.data_locale);
    if (object->usage == "search") id.keywords.emplace_back("co", "search");
    else if (object->collation != "default") id.keywords.emplace_back("co", object->collation);
    if (object->numeric) id.keywords.emplace_back("kn", "true");
    if (object->case_first != "false") id.keywords.emplace_back("kf", object->case_first);
    std::sort(id.keywords.begin(), id.keywords.end());
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(id.to_string());
    if (!locale) { ctx.throw_range_error("Incorrect locale information provided"); return Value(); }

    icu4x::CollatorOptionsV1 icu_options;
    const std::string& s = object->sensitivity;
    icu_options.strength = s == "base" || s == "case" ? icu4x::CollatorStrength::Primary
        : s == "accent" ? icu4x::CollatorStrength::Secondary : icu4x::CollatorStrength::Tertiary;
    if (s == "case") icu_options.case_level = icu4x::CollatorCaseLevel::On;
    icu_options.alternate_handling = object->ignore_punctuation ? icu4x::CollatorAlternateHandling::Shifted
                                                                : icu4x::CollatorAlternateHandling::NonIgnorable;
    auto created = icu4x::Collator::create_v1_with_provider(IntlICU::provider(), *locale, icu_options);
    if (!created.is_ok()) { ctx.throw_range_error("Collation is not available for this locale"); return Value(); }
    object->collator = std::move(created).ok().value();
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

}  // namespace

void register_collator(Context& ctx, Object* intl) {
    Class collator = define_class(ctx, intl, "Collator", 0, Kind::Collator, construct_collator);
    add_supported_locales_of(ctx, collator.constructor);
    Object* proto = collator.prototype;

    define_getter(proto, "compare", [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<CollatorObject>(ctx, receiver, "Collator");
        if (!self) return Value();
        if (self->bound_compare.Get().is_undefined()) {
            self->bound_compare.Set(make_bound_function(ctx, self, 2,
                [](Context& ctx, std::span<const Value> args, DOMObject* owner) -> Value {
                    auto* collator = static_cast<CollatorObject*>(owner);
                    std::string x, y;
                    if (!(args.size() > 0 ? args[0] : Value()).to_string_checked(ctx, x)) return Value();
                    if (!(args.size() > 1 ? args[1] : Value()).to_string_checked(ctx, y)) return Value();
                    return Value(static_cast<double>(collator->collator->compare(x, y)));
                }));
        }
        return self->bound_compare.Get();
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<CollatorObject>(ctx, receiver, "Collator");
        if (!self) return Value();
        return Value(make_object({
            {"locale", Value(self->locale)},
            {"usage", Value(self->usage)},
            {"sensitivity", Value(self->sensitivity)},
            {"ignorePunctuation", Value(self->ignore_punctuation)},
            {"collation", Value(self->collation)},
            {"numeric", Value(self->numeric)},
            {"caseFirst", Value(self->case_first)},
        }));
    });
}

bool locale_compare(Context& ctx, const std::string& a, const std::string& b, const Value& locales, const Value& options, double& out) {
    const Value arguments[2] = {locales, options};
    Value created = construct_collator(ctx, arguments, Value(), true, Value());
    if (ctx.has_exception()) return false;
    CollatorObject* collator = DOMObject::Cast<CollatorObject>(created);
    if (!collator) return false;
    out = static_cast<double>(collator->collator->compare(a, b));
    return true;
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
