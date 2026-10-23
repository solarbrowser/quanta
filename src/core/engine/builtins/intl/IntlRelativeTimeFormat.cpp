/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include <cmath>

namespace Quanta::IntlImpl {

namespace {

struct RelativeTimeObject : DOMObject {
    std::string locale, style = "long", numeric = "always", numbering_system;
    std::string icu_locale;
    Symbols symbols;
    std::unique_ptr<icu4x::DecimalFormatter> formatter;
};

// SingularRelativeTimeUnit: the unit as one of the eight, or -1.
int unit_index(const std::string& unit) {
    static const char* const names[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
    for (int i = 0; i < 8; i++) {
        if (unit == names[i] || unit == std::string(names[i]) + "s") return i;
    }
    return -1;
}

Value construct_relative_time(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.RelativeTimeFormat requires 'new'"); return Value(); }
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
    Object* options = coerce_options_to_object(ctx, args.size() > 1 ? args[1] : Value());
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
    std::optional<std::string> style, numeric;
    if (!get_string_option(ctx, options, "style", {"long", "short", "narrow"}, style)) return Value();
    if (!get_string_option(ctx, options, "numeric", {"always", "auto"}, numeric)) return Value();
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::RelativeTimeFormat);
    if (ctx.has_exception()) return Value();

    auto* object = Heap::Allocate<RelativeTimeObject>();
    object->locale = resolved.locale;
    object->style = style ? *style : "long";
    object->numeric = numeric ? *numeric : "always";
    object->numbering_system = *resolved.value_of("nu");
    LocaleId id = IntlICU::split_tag(resolved.data_locale);
    id.keywords.emplace_back("nu", object->numbering_system);
    object->icu_locale = id.to_string();
    object->formatter = make_formatter(object->icu_locale, icu4x::DecimalGroupingStrategy::Auto);
    if (!object->formatter) { ctx.throw_range_error("Relative time formatting is not available for this locale"); return Value(); }
    object->symbols = learn_symbols(object->icu_locale);
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

// PartitionRelativeTimePattern. False with an exception pending.
bool partition(Context& ctx, RelativeTimeObject& self, std::span<const Value> args, std::vector<Part>& parts, std::string& unit_name) {
    double value;
    if (!(args.size() > 0 ? args[0] : Value()).to_number_checked(ctx, value)) return false;
    std::string unit;
    if (!(args.size() > 1 ? args[1] : Value()).to_string_checked(ctx, unit)) return false;
    const int index = unit_index(unit);
    if (index < 0) return range_error(ctx, "Invalid unit argument for format() '" + unit + "'");
    if (!std::isfinite(value)) return range_error(ctx, "Value need to be finite number for Intl.RelativeTimeFormat.prototype.format()");
    static const char* const singular[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
    unit_name = singular[index];

    Numeric number = numeric_from_double(value);
    // With words, a zero of either sign is "now" ("this year"): the sign only matters to the numbers.
    const std::string signed_text = value == 0 && self.numeric == "auto" ? std::string("0") : number.value->to_string();
    const int style = self.style == "long" ? 0 : self.style == "short" ? 1 : 2;
    std::optional<std::string> text = IntlICU::shim::relative_time(self.icu_locale, signed_text, index, style, self.numeric == "auto");
    if (!text) return range_error(ctx, "Relative time formatting is not available");

    auto magnitude = copy_of(*number.value);
    magnitude->set_sign(icu4x::DecimalSign::None);
    const std::string digits = self.formatter->format(*magnitude);
    const size_t at = text->find(digits);
    if (at == std::string::npos) {
        parts.push_back({"literal", *text});   // "tomorrow": no number in it
        return true;
    }
    if (at > 0) parts.push_back({"literal", text->substr(0, at)});
    std::vector<Part> numeric_parts;
    number_parts(self.symbols, digits, numeric_parts);
    for (Part& p : numeric_parts) parts.push_back(std::move(p));
    for (size_t i = (at > 0 ? 1 : 0); i < parts.size(); i++) parts[i].unit = unit_name;
    if (at + digits.size() < text->size()) parts.push_back({"literal", text->substr(at + digits.size())});
    return true;
}

}  // namespace

void register_relative_time_format(Context& ctx, Object* intl) {
    Class relative = define_class(ctx, intl, "RelativeTimeFormat", 0, Kind::RelativeTimeFormat, construct_relative_time);
    add_supported_locales_of(ctx, relative.constructor);
    Object* proto = relative.prototype;

    define_method(proto, "format", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<RelativeTimeObject>(ctx, receiver, "RelativeTimeFormat");
        if (!self) return Value();
        std::vector<Part> parts;
        std::string unit;
        if (!partition(ctx, *self, args, parts, unit)) return Value();
        std::string out;
        for (const Part& p : parts) out += p.value;
        return Value(out);
    });

    define_method(proto, "formatToParts", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<RelativeTimeObject>(ctx, receiver, "RelativeTimeFormat");
        if (!self) return Value();
        std::vector<Part> parts;
        std::string unit;
        if (!partition(ctx, *self, args, parts, unit)) return Value();
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
        auto* self = this_as<RelativeTimeObject>(ctx, receiver, "RelativeTimeFormat");
        if (!self) return Value();
        return Value(make_object({
            {"locale", Value(self->locale)}, {"style", Value(self->style)}, {"numeric", Value(self->numeric)},
            {"numberingSystem", Value(self->numbering_system)},
        }));
    });
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
