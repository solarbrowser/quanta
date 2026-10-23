/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include "quanta/core/runtime/Symbol.h"

#include "icu4x/DecimalFormatter.hpp"
#include "icu4x/DecimalGroupingStrategy.hpp"
#include "icu4x/DecimalSignDisplay.hpp"
#include "icu4x/Locale.hpp"
#include "icu4x/PluralCategory.hpp"
#include "icu4x/PluralOperands.hpp"
#include "icu4x/PluralRules.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Quanta::IntlImpl {

namespace {

struct NumberFormatObject : DOMObject {
    std::string locale, data_locale, numbering_system;
    std::string style = "decimal";
    std::string currency, currency_display = "symbol", currency_sign = "standard";
    std::string unit, unit_display = "short";
    std::string notation = "standard", compact_display = "short";
    std::string use_grouping = "auto";   // "auto", "always", "min2" or "false"
    std::string sign_display = "auto";
    DigitOptions digits;
    std::unique_ptr<icu4x::DecimalFormatter> formatter;
    std::unique_ptr<icu4x::PluralRules> plural_rules;
    std::string icu_locale;               // data_locale with -u-nu-
    Symbols symbols;
    bool transliterate = false;           // the data has no digits for this numbering system: map them here
    TracedValue bound_format;
};

// ---- Locale data ----------------------------------------------------------------------------------------------------

bool is_space_like(char32_t c) {
    return c == ' ' || c == 0xA0 || c == 0x202F || c == 0x2009 || c == 0x200A || c == 0x2007 || c == 0x3000;
}

char32_t next_code_point(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    char32_t cp = length == 1 ? c : c & (0xFF >> (length + 1));
    for (size_t k = 1; k < length && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += length;
    return cp;
}

// The text around the number, cut into signs, spaces and the one kind of affix the style has.
void affix_parts(const NumberFormatObject& nf, const std::string& text, const char* type, std::vector<Part>& out) {
    size_t i = 0;
    auto push = [&](const std::string& kind, const std::string& value) {
        if (!out.empty() && out.back().type == kind && kind != "minusSign" && kind != "plusSign") out.back().value += value;
        else out.push_back({kind, value});
    };
    while (i < text.size()) {
        if (!nf.symbols.minus.empty() && text.compare(i, nf.symbols.minus.size(), nf.symbols.minus) == 0) {
            push("minusSign", nf.symbols.minus);
            i += nf.symbols.minus.size();
            continue;
        }
        if (!nf.symbols.plus.empty() && text.compare(i, nf.symbols.plus.size(), nf.symbols.plus) == 0) {
            push("plusSign", nf.symbols.plus);
            i += nf.symbols.plus.size();
            continue;
        }
        const size_t start = i;
        const char32_t cp = next_code_point(text, i);
        const std::string piece = text.substr(start, i - start);
        if (is_space_like(cp) || cp == '(' || cp == ')' || cp == 0x200E || cp == 0x200F || cp == 0x061C) push("literal", piece);
        else push(type, piece);
    }
}

bool plural_is_same(NumberFormatObject& nf, const icu4x::Decimal& a, const icu4x::Decimal& b) {
    if (!nf.plural_rules) {
        std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(nf.data_locale);
        auto made = icu4x::PluralRules::create_cardinal_with_provider(IntlICU::provider(), *locale);
        if (!made.is_ok()) return true;
        nf.plural_rules = std::move(made).ok().value();
    }
    return nf.plural_rules->category_for(*icu4x::PluralOperands::from_fixed_decimal(a)) ==
           nf.plural_rules->category_for(*icu4x::PluralOperands::from_fixed_decimal(b));
}

// The units the data has no names for, written the English way.
struct UnitFallback {
    const char* unit;
    const char* short_form;
    const char* narrow_form;
    const char* long_one;
    const char* long_other;
};
const UnitFallback kUnitFallbacks[] = {
    {"bit", " bit", "bit", " bit", " bits"}, {"byte", " byte", "B", " byte", " bytes"},
    {"kilobit", " kb", "kb", " kilobit", " kilobits"}, {"kilobyte", " kB", "kB", " kilobyte", " kilobytes"},
    {"megabit", " Mb", "Mb", " megabit", " megabits"}, {"megabyte", " MB", "MB", " megabyte", " megabytes"},
    {"gigabit", " Gb", "Gb", " gigabit", " gigabits"}, {"gigabyte", " GB", "GB", " gigabyte", " gigabytes"},
    {"terabit", " Tb", "Tb", " terabit", " terabits"}, {"terabyte", " TB", "TB", " terabyte", " terabytes"},
    {"petabyte", " PB", "PB", " petabyte", " petabytes"},
    {"celsius", "\xC2\xB0" "C", "\xC2\xB0" "C", " degree Celsius", " degrees Celsius"},
    {"fahrenheit", "\xC2\xB0" "F", "\xC2\xB0", " degree Fahrenheit", " degrees Fahrenheit"},
    {"degree", " deg", "\xC2\xB0", " degree", " degrees"}, {"percent", "%", "%", " percent", " percent"},
};

std::string unit_text(NumberFormatObject& nf, const std::string& number_text, const icu4x::Decimal& value) {
    const int width = nf.unit_display == "long" ? 0 : nf.unit_display == "short" ? 1 : 2;
    if (std::optional<std::string> text = IntlICU::shim::unit(nf.icu_locale, number_text, value.to_string(), nf.unit, width)) {
        // CLDR writes the speeds with the Latin abbreviations as one word; composing the two halves gives "km/hr".
        auto ends_with = [&](const char* tail) {
            const std::string t = tail;
            return text->size() >= t.size() && text->compare(text->size() - t.size(), t.size(), t) == 0;
        };
        if (width != 0 && nf.unit.find("-per-") != std::string::npos) {
            if (ends_with("/hr")) text->replace(text->size() - 2, 2, "h");
            else if (ends_with("/sec")) text->replace(text->size() - 3, 3, "s");
            if (nf.unit == "mile-per-hour" && IntlICU::split_tag(nf.locale).language == "en" && ends_with("mi/h")) {
                text->replace(text->size() - 4, 4, "mph");
            }
        }
        return *text;
    }
    // Compound units whose halves the fallbacks know are not handled; a unit with no name at all keeps its identifier.
    for (const UnitFallback& f : kUnitFallbacks) {
        if (nf.unit != f.unit) continue;
        if (width == 1) return number_text + f.short_form;
        if (width == 2) return number_text + f.narrow_form;
        auto one = icu4x::Decimal::from(1);
        return number_text + (plural_is_same(nf, value, *one) ? f.long_one : f.long_other);
    }
    return number_text + " " + nf.unit;
}

// ---- Formatting -----------------------------------------------------------------------------------------------------

std::string digits_only(const icu4x::Decimal& d) {
    auto positive = copy_of(d);
    positive->set_sign(icu4x::DecimalSign::None);
    return positive->to_string();
}

// The text a shim formatter puts before and after a number: format a whole number that picks the same plural form as
// `reference` (so "1 US dollar" and "2 US dollars" are told apart) and cut the number out of the result. The shim
// formatters pad and round on their own, so the digits of the real number are put in by the caller.
bool probe_affixes(NumberFormatObject& nf, const icu4x::Decimal& reference, int fraction_digits, icu4x::DecimalSign sign,
                   const std::function<std::optional<std::string>(const std::string&)>& format, std::string& prefix,
                   std::string& suffix) {
    for (int j = 1; j < 1000; j++) {
        auto candidate = icu4x::Decimal::from(j);
        if (!plural_is_same(nf, *candidate, reference)) continue;
        candidate->pad_end(static_cast<int16_t>(-fraction_digits));
        auto probe = copy_of(*candidate);
        probe->set_sign(sign);
        std::optional<std::string> text = format(probe->to_string());
        if (!text) return false;
        std::string digits = nf.formatter->format(*candidate);
        const size_t at = text->find(digits);
        if (at == std::string::npos) return false;
        prefix = text->substr(0, at);
        suffix = text->substr(at + digits.size());
        return true;
    }
    return false;
}

// ComputeExponentForMagnitude for compact notation: how far the locale scales a number of this magnitude, found by
// formatting powers of ten and counting the digits left of the suffix.
int compact_exponent(NumberFormatObject& nf, int magnitude) {
    if (magnitude < 3) return 0;
    const bool long_form = nf.compact_display == "long";
    const std::string probe = "1" + std::string(static_cast<size_t>(magnitude), '0');
    std::optional<std::string> text = IntlICU::shim::compact(nf.icu_locale, probe, long_form);
    if (!text) return 0;
    // The digits the locale writes the probe with, wherever they stand: a number that was not scaled has all of its own.
    size_t digits = 0;
    for (size_t i = 0; i < text->size();) {
        bool is_digit = false;
        for (const std::string& d : nf.symbols.digit) {
            if (!d.empty() && text->compare(i, d.size(), d) == 0) { i += d.size(); digits++; is_digit = true; break; }
        }
        if (!is_digit) {
            if ((*text)[i] >= '0' && (*text)[i] <= '9') digits++;
            i++;
        }
    }
    if (digits == 0) return 0;
    return std::max(0, magnitude - static_cast<int>(digits) + 1);
}

int exponent_for(NumberFormatObject& nf, int magnitude) {
    if (nf.notation == "scientific") return magnitude;
    if (nf.notation == "engineering") return static_cast<int>(std::floor(magnitude / 3.0)) * 3;
    if (nf.notation == "compact") return compact_exponent(nf, magnitude);
    return 0;
}

struct Rendered {
    std::unique_ptr<icu4x::Decimal> value;   // rounded, signed per signDisplay
    int exponent = 0;
};

// FormatNumericToString + the exponent of the notation + the sign rules.
Rendered render(NumberFormatObject& nf, const Numeric& number) {
    Rendered r;
    r.value = copy_of(*number.value);
    const bool negative = number.negative();
    if (nf.style == "percent") r.value->multiply_pow10(2);
    if (nf.notation != "standard" && !r.value->is_zero()) {
        const int magnitude = r.value->nonzero_magnitude_start();
        r.exponent = exponent_for(nf, magnitude);
        auto scaled = copy_of(*r.value);
        scaled->multiply_pow10(static_cast<int16_t>(-r.exponent));
        apply_digit_options(nf.digits, *scaled);
        // Rounding carried into the next order of magnitude (999999 -> 1000K): scale again from there.
        if (!scaled->is_zero() && scaled->nonzero_magnitude_start() != magnitude - r.exponent) {
            r.exponent = exponent_for(nf, magnitude + 1);
            scaled = copy_of(*r.value);
            scaled->multiply_pow10(static_cast<int16_t>(-r.exponent));
            apply_digit_options(nf.digits, *scaled);
        }
        r.value = std::move(scaled);
    } else {
        apply_digit_options(nf.digits, *r.value);
    }
    r.value->set_sign(negative ? icu4x::DecimalSign::Negative : icu4x::DecimalSign::None);
    const std::string& s = nf.sign_display;
    r.value->apply_sign_display(s == "never" ? icu4x::DecimalSignDisplay::Never : s == "always" ? icu4x::DecimalSignDisplay::Always
        : s == "exceptZero" ? icu4x::DecimalSignDisplay::ExceptZero : s == "negative" ? icu4x::DecimalSignDisplay::Negative
        : icu4x::DecimalSignDisplay::Auto);
    return r;
}

std::vector<Part> format_parts(NumberFormatObject& nf, const Numeric& number) {
    std::vector<Part> parts;
    if (number.kind != Numeric::Kind::Finite) {
        // Sign by signDisplay on the infinities and NaN.
        const bool negative = number.negative();
        const std::string& s = nf.sign_display;
        const bool show_minus = negative && s != "never";
        const bool show_plus = !negative && number.kind != Numeric::Kind::NaN && (s == "always" || s == "exceptZero");
        const bool nan_plus = number.kind == Numeric::Kind::NaN && s == "always";
        if (show_minus) parts.push_back({"minusSign", nf.symbols.minus});
        if (show_plus || nan_plus) parts.push_back({"plusSign", nf.symbols.plus});
        parts.push_back({number.kind == Numeric::Kind::NaN ? "nan" : "infinity", number.kind == Numeric::Kind::NaN ? "NaN" : "\xE2\x88\x9E"});
        if (nf.style == "percent") parts.push_back({"percentSign", "%"});
        return parts;
    }

    Rendered r = render(nf, number);
    std::string number_text_mutable = nf.formatter->format(*([&] {
        auto d = copy_of(*r.value);
        d->set_sign(icu4x::DecimalSign::None);
        return d;
    })());
    const std::string& number_text = number_text_mutable;

    std::string whole;                 // the whole text with the style's affixes
    std::string number_core = number_text;
    if (nf.style == "percent") {
        whole = IntlICU::shim::percent(nf.icu_locale, r.value->to_string()).value_or(number_text + "%");
    } else if (nf.style == "currency") {
        const int width = nf.currency_display == "symbol" ? 0 : nf.currency_display == "narrowSymbol" ? 1
            : nf.currency_display == "code" ? 2 : 3;
        auto magnitude = copy_of(*r.value);
        magnitude->set_sign(icu4x::DecimalSign::None);
        std::string prefix, suffix;
        const bool found = probe_affixes(nf, *magnitude, currency_digits(nf.currency), r.value->sign(),
            [&](const std::string& decimal) {
                return IntlICU::shim::currency(nf.icu_locale, decimal, nf.currency, width, nf.currency_sign == "accounting");
            }, prefix, suffix);
        whole = found ? prefix + number_text + suffix : nf.currency + "\xC2\xA0" + number_text;
    } else if (nf.style == "unit") {
        auto magnitude = copy_of(*r.value);
        magnitude->set_sign(icu4x::DecimalSign::None);
        const std::string signed_text = nf.formatter->format(*r.value);
        // The sign goes in front of the whole, the way the units' own patterns leave it.
        whole = unit_text(nf, number_text, *magnitude);
        const std::string sign_only = signed_text.substr(0, signed_text.find(number_text));
        whole = sign_only + whole;
    } else {
        whole = nf.formatter->format(*r.value);
    }

    if (nf.transliterate) {
        auto translate = [&](std::string& text) {
            std::string out;
            for (char c : text) {
                if (c >= '0' && c <= '9') out += nf.symbols.digit[c - '0'];
                else out += c;
            }
            text = std::move(out);
        };
        translate(number_text_mutable);
        translate(whole);
        translate(number_core);
    }

    // The compact suffix and the exponent come after the number.
    std::string compact_prefix, compact_suffix;
    if (nf.notation == "compact" && r.exponent > 0) {
        // The pattern of this magnitude and plural form: format a whole number that selects the same one and take
        // what stands around its digits.
        auto mantissa = copy_of(*r.value);
        mantissa->set_sign(icu4x::DecimalSign::None);
        for (int j = 1; j < 1000; j++) {
            auto candidate = icu4x::Decimal::from(j);
            if (!plural_is_same(nf, *candidate, *mantissa)) continue;
            auto scaled = copy_of(*candidate);
            scaled->multiply_pow10(static_cast<int16_t>(r.exponent));
            std::optional<std::string> sample = IntlICU::shim::compact(nf.icu_locale, scaled->to_string(), nf.compact_display == "long");
            const std::string digits = nf.formatter->format(*candidate);
            if (!sample) break;
            const size_t at = sample->find(digits);
            if (at == std::string::npos) break;
            compact_prefix = sample->substr(0, at);
            compact_suffix = sample->substr(at + digits.size());
            break;
        }
    }

    if (nf.notation == "scientific" || nf.notation == "engineering") {
        // number E exponent
    }

    // Locate the number inside the whole text; what is around it is the style's affix and the sign.
    std::string around_before, around_after;
    if (nf.style == "decimal") {
        const size_t at = whole.find(number_text);
        if (at != std::string::npos) { around_before = whole.substr(0, at); around_after = whole.substr(at + number_text.size()); }
    } else {
        const size_t at = whole.find(number_text);
        if (at != std::string::npos) { around_before = whole.substr(0, at); around_after = whole.substr(at + number_text.size()); }
        else { around_before = ""; around_after = ""; number_core = whole; }
    }
    const char* kind = nf.style == "percent" ? "percentSign" : nf.style == "currency" ? "currency" : "unit";
    affix_parts(nf, around_before, kind, parts);
    if (!compact_prefix.empty()) affix_parts(nf, compact_prefix, "compact", parts);
    number_parts(nf.symbols, number_core, parts);
    if (nf.notation == "scientific" || nf.notation == "engineering") {
        parts.push_back({"exponentSeparator", "E"});
        std::string exponent = std::to_string(std::abs(r.exponent));
        if (r.exponent < 0) parts.push_back({"exponentMinusSign", nf.symbols.minus.empty() ? "-" : nf.symbols.minus});
        std::string localized;
        for (char c : exponent) localized += nf.symbols.digit[c - '0'];
        parts.push_back({"exponentInteger", localized});
    }
    if (!compact_suffix.empty()) affix_parts(nf, compact_suffix, "compact", parts);
    affix_parts(nf, around_after, kind, parts);
    return parts;
}

// ---- Ranges ---------------------------------------------------------------------------------------------------------

bool is_number_part(const std::string& type) {
    return type == "integer" || type == "fraction" || type == "group" || type == "decimal" || type == "exponentSeparator" ||
           type == "exponentInteger" || type == "exponentMinusSign" || type == "compact" || type == "nan" || type == "infinity";
}

size_t code_points(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// FormatNumericRange: the two ends with what they share written once, the way ICU's number range formatter does: the
// signs and the currency or unit around the numbers are collapsed when they are the same on both sides and more than one
// code point long; the identical ends become "~" and one number.
bool range_parts(Context& ctx, NumberFormatObject& nf, std::span<const Value> args, std::vector<Part>& out) {
    const Value start = args.size() > 0 ? args[0] : Value();
    const Value end = args.size() > 1 ? args[1] : Value();
    if (start.is_undefined() || end.is_undefined()) return type_error(ctx, "start and end must be provided");
    Numeric x, y;
    if (!to_intl_numeric(ctx, start, x) || !to_intl_numeric(ctx, end, y)) return false;
    if (x.kind == Numeric::Kind::NaN || y.kind == Numeric::Kind::NaN) return range_error(ctx, "start and end must not be NaN");
    std::vector<Part> a = format_parts(nf, x), b = format_parts(nf, y);
    auto text_of = [](const std::vector<Part>& parts) {
        std::string t;
        for (const Part& p : parts) t += p.value;
        return t;
    };
    if (text_of(a) == text_of(b)) {
        out.push_back({"approximatelySign", "~", "", "shared"});
        for (Part& p : a) { p.source = "shared"; out.push_back(std::move(p)); }
        return true;
    }
    // Where the number begins and ends inside each side.
    auto bounds = [](const std::vector<Part>& parts) {
        size_t first = parts.size(), last = 0;
        for (size_t i = 0; i < parts.size(); i++) {
            if (is_number_part(parts[i].type)) { first = std::min(first, i); last = i + 1; }
        }
        if (first == parts.size()) first = last = 0;
        return std::make_pair(first, last);
    };
    auto [a_first, a_last] = bounds(a);
    auto [b_first, b_last] = bounds(b);
    auto slice = [](const std::vector<Part>& parts, size_t from, size_t to) {
        std::string key;
        for (size_t i = from; i < to; i++) key += parts[i].type + "\x01" + parts[i].value + "\x02";
        return key;
    };
    const bool same_prefix = slice(a, 0, a_first) == slice(b, 0, b_first);
    const bool same_suffix = slice(a, a_last, a.size()) == slice(b, b_last, b.size());
    std::string affix_text;
    for (size_t i = 0; i < a_first; i++) affix_text += a[i].value;
    for (size_t i = a_last; i < a.size(); i++) affix_text += a[i].value;
    const bool collapse = same_prefix && same_suffix && code_points(affix_text) > 1;

    const std::string language = IntlICU::split_tag(nf.locale).language;
    std::string separator = language == "pt" ? " - " : "\xE2\x80\x93";
    if (!collapse && (a_first > 0 || a_last < a.size() || b_first > 0 || b_last < b.size()) && separator == "\xE2\x80\x93") {
        separator = " \xE2\x80\x93 ";
    }
    auto push = [&](const std::vector<Part>& parts, size_t from, size_t to, const char* source) {
        for (size_t i = from; i < to; i++) { Part p = parts[i]; p.source = source; out.push_back(std::move(p)); }
    };
    if (collapse) {
        push(a, 0, a_first, "shared");
        push(a, a_first, a_last, "startRange");
        out.push_back({"literal", separator, "", "shared"});
        push(b, b_first, b_last, "endRange");
        push(a, a_last, a.size(), "shared");
    } else {
        push(a, 0, a.size(), "startRange");
        out.push_back({"literal", separator, "", "shared"});
        push(b, 0, b.size(), "endRange");
    }
    return true;
}

std::string join(const std::vector<Part>& parts) {
    std::string out;
    for (const Part& p : parts) out += p.value;
    return out;
}

// OrdinaryHasInstance(%NumberFormat%, object): through the prototype chain, asking a Proxy's own getPrototypeOf.
bool inherits_from(Object* object, Object* proto) {
    for (Object* p = object ? object->get_prototype() : nullptr; p; p = p->get_prototype()) {
        if (p == proto) return true;
    }
    return false;
}

// ChainNumberFormat: called as a function on an object that inherits from NumberFormat.prototype, the new format is
// parked on that object under the fallback symbol and the object is what comes back.
Value chain_legacy(Context& ctx, const Value& this_value, NumberFormatObject* created) {
    Object* self = this_value.is_function() ? static_cast<Object*>(this_value.as_function()) : this_value.as_object_or_null();
    Object* proto = prototype_of(ctx, Kind::NumberFormat);
    if (self && proto && inherits_from(self, proto)) {
        PropertyDescriptor descriptor(Value(static_cast<Object*>(created)), static_cast<PropertyAttributes>(0));
        self->set_property_descriptor(legacy_fallback_symbol(ctx)->to_property_key(), descriptor);
        return this_value;
    }
    return Value(static_cast<Object*>(created));
}

// UnwrapNumberFormat
NumberFormatObject* unwrap(Context& ctx, const Value& receiver) {
    if (NumberFormatObject* direct = DOMObject::Cast<NumberFormatObject>(receiver)) return direct;
    Object* self = receiver.is_function() ? static_cast<Object*>(receiver.as_function()) : receiver.as_object_or_null();
    Object* proto = prototype_of(ctx, Kind::NumberFormat);
    if (self && proto && inherits_from(self, proto)) {
        Value inner = self->get_property(legacy_fallback_symbol(ctx)->to_property_key());
        if (ctx.has_exception()) return nullptr;
        if (NumberFormatObject* found = DOMObject::Cast<NumberFormatObject>(inner)) return found;
    }
    ctx.throw_type_error("Method called on an incompatible receiver, not an Intl.NumberFormat");
    return nullptr;
}

// ---- The constructor --------------------------------------------------------------------------------------------------

Value construct_number_format(Context& ctx, std::span<const Value> args, Value this_value, bool is_construct, Value new_target) {
    std::vector<std::string> requested;
    Object* options;
    if (!read_locales_and_options(ctx, args, requested, options)) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> numbering;
    if (!get_string_option(ctx, options, "numberingSystem", {}, numbering)) return Value();
    if (numbering) {
        // type = (3*8alphanum) *("-" (3*8alphanum))
        bool valid = !numbering->empty();
        size_t start = 0;
        while (valid && start <= numbering->size()) {
            size_t dash = numbering->find('-', start);
            size_t end = dash == std::string::npos ? numbering->size() : dash;
            if (end - start < 3 || end - start > 8) valid = false;
            for (size_t i = start; valid && i < end; i++) valid = std::isalnum(static_cast<unsigned char>((*numbering)[i])) != 0;
            if (dash == std::string::npos) break;
            start = dash + 1;
        }
        if (!valid) { ctx.throw_range_error("Invalid numberingSystem"); return Value(); }
    }
    std::vector<std::pair<std::string, std::string>> overrides;
    if (numbering) {
        std::string lower = *numbering;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        overrides.emplace_back("nu", lower);
    }
    Resolved resolved = resolve_locale(requested, {{"nu", numbering_system_values}}, overrides, matcher);

    auto* object = Heap::Allocate<NumberFormatObject>();
    NumberFormatObject& nf = *object;
    nf.locale = resolved.locale;
    nf.data_locale = resolved.data_locale;
    nf.numbering_system = *resolved.value_of("nu");

    // SetNumberFormatUnitOptions
    std::optional<std::string> style, currency, currency_display, currency_sign, unit, unit_display;
    if (!get_string_option(ctx, options, "style", {"decimal", "percent", "currency", "unit"}, style)) return Value();
    nf.style = style ? *style : "decimal";
    if (!get_string_option(ctx, options, "currency", {}, currency)) return Value();
    if (currency) {
        if (currency->size() != 3 || !std::all_of(currency->begin(), currency->end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; })) {
            ctx.throw_range_error("Invalid currency code"); return Value();
        }
    }
    if (nf.style == "currency" && !currency) { ctx.throw_type_error("Currency code is required with currency style."); return Value(); }
    if (!get_string_option(ctx, options, "currencyDisplay", {"code", "symbol", "narrowSymbol", "name"}, currency_display)) return Value();
    if (!get_string_option(ctx, options, "currencySign", {"standard", "accounting"}, currency_sign)) return Value();
    if (!get_string_option(ctx, options, "unit", {}, unit)) return Value();
    if (unit && !is_well_formed_unit(*unit)) { ctx.throw_range_error("Invalid unit argument"); return Value(); }
    if (nf.style == "unit" && !unit) { ctx.throw_type_error("Unit is required with unit style."); return Value(); }
    if (!get_string_option(ctx, options, "unitDisplay", {"short", "narrow", "long"}, unit_display)) return Value();
    if (nf.style == "currency") {
        nf.currency = *currency;
        for (char& c : nf.currency) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        nf.currency_display = currency_display ? *currency_display : "symbol";
        nf.currency_sign = currency_sign ? *currency_sign : "standard";
    }
    if (nf.style == "unit") {
        nf.unit = *unit;
        nf.unit_display = unit_display ? *unit_display : "short";
    }

    std::optional<std::string> notation, compact_display;
    if (!get_string_option(ctx, options, "notation", {"standard", "scientific", "engineering", "compact"}, notation)) return Value();
    nf.notation = notation ? *notation : "standard";
    int minimum_fraction = 0, maximum_fraction = 3;
    if (nf.style == "currency" && nf.notation == "standard") minimum_fraction = maximum_fraction = currency_digits(nf.currency);
    else if (nf.style == "percent") maximum_fraction = 0;
    if (!read_digit_options(ctx, options, minimum_fraction, maximum_fraction, nf.notation == "compact", nf.digits)) return Value();
    if (!get_string_option(ctx, options, "compactDisplay", {"short", "long"}, compact_display)) return Value();
    nf.compact_display = compact_display ? *compact_display : "short";

    // useGrouping: a boolean or one of the strings
    std::string default_grouping = nf.notation == "compact" ? "min2" : "auto";
    nf.use_grouping = default_grouping;
    {
        Value value = options->get_property("useGrouping");
        if (ctx.has_exception()) return Value();
        if (!value.is_undefined()) {
            if (value.is_boolean()) {
                nf.use_grouping = value.to_boolean() ? "always" : "false";
            } else if (value.is_null() || !value.to_boolean()) {
                nf.use_grouping = "false";
            } else {
                std::string text;
                if (!value.to_string_checked(ctx, text)) return Value();
                if (text == "true" || text == "false") nf.use_grouping = default_grouping;
                else if (text == "min2" || text == "auto" || text == "always") nf.use_grouping = text;
                else { ctx.throw_range_error("Value out of range for option useGrouping"); return Value(); }
            }
        }
    }
    std::optional<std::string> sign_display;
    if (!get_string_option(ctx, options, "signDisplay", {"auto", "never", "always", "exceptZero", "negative"}, sign_display)) return Value();
    nf.sign_display = sign_display ? *sign_display : "auto";

    Object* proto = prototype_from_new_target(ctx, is_construct ? new_target : Value(), Kind::NumberFormat);
    if (ctx.has_exception()) return Value();

    // The formatter reads the numbering system from the locale's own keyword.
    LocaleId id = IntlICU::split_tag(nf.locale);
    id.keywords.clear();
    id.keywords.emplace_back("nu", nf.numbering_system);
    nf.icu_locale = id.to_string();
    const icu4x::DecimalGroupingStrategy strategy = nf.use_grouping == "always" ? icu4x::DecimalGroupingStrategy::Always
        : nf.use_grouping == "min2" ? icu4x::DecimalGroupingStrategy::Min2
        : nf.use_grouping == "false" ? icu4x::DecimalGroupingStrategy::Never : icu4x::DecimalGroupingStrategy::Auto;
    nf.formatter = make_formatter(nf.icu_locale, strategy);
    if (!nf.formatter) { ctx.throw_range_error("Number formatting is not available for this locale"); return Value(); }
    nf.symbols = learn_symbols(nf.icu_locale);
    if (nf.symbols.digit[0] == "0" && nf.numbering_system != "latn") {
        const std::string_view system = numbering_system_digits(nf.numbering_system);
        if (!system.empty()) {
            size_t at = 0;
            for (int d = 0; d < 10 && at < system.size(); d++) {
                size_t length = 1;
                const unsigned char c = static_cast<unsigned char>(system[at]);
                length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                nf.symbols.digit[d] = std::string(system.substr(at, length));
                at += length;
            }
            nf.transliterate = true;
        }
    }
    if (proto) object->initialize_prototype(proto);
    if (!is_construct) return chain_legacy(ctx, this_value, object);
    return Value(static_cast<Object*>(object));
}

Value parts_array(const std::vector<Part>& parts) {
    auto array = ObjectFactory::create_array(0);
    uint32_t index = 0;
    for (const Part& p : parts) {
        array->set_element(index++, Value(make_object({{"type", Value(p.type)}, {"value", Value(p.value)}})));
    }
    return Value(array.release());
}

}  // namespace

void register_number_format(Context& ctx, Object* intl) {
    Class number = define_class(ctx, intl, "NumberFormat", 0, Kind::NumberFormat, construct_number_format);
    add_supported_locales_of(ctx, number.constructor);
    Object* proto = number.prototype;

    define_getter(proto, "format", [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        NumberFormatObject* self = unwrap(ctx, receiver);
        if (!self) return Value();
        if (self->bound_format.Get().is_undefined()) {
            self->bound_format.Set(make_bound_function(ctx, self, 1,
                [](Context& ctx, std::span<const Value> args, DOMObject* owner) -> Value {
                    Numeric n;
                    if (!to_intl_numeric(ctx, args.empty() ? Value() : args[0], n)) return Value();
                    return Value(join(format_parts(*static_cast<NumberFormatObject*>(owner), n)));
                }));
        }
        return self->bound_format.Get();
    });

    define_method(proto, "formatToParts", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<NumberFormatObject>(ctx, receiver, "NumberFormat");
        if (!self) return Value();
        Numeric n;
        if (!to_intl_numeric(ctx, args.empty() ? Value() : args[0], n)) return Value();
        return parts_array(format_parts(*self, n));
    });

    define_method(proto, "formatRange", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<NumberFormatObject>(ctx, receiver, "NumberFormat");
        if (!self) return Value();
        std::vector<Part> parts;
        if (!range_parts(ctx, *self, args, parts)) return Value();
        return Value(join(parts));
    });

    define_method(proto, "formatRangeToParts", 2, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<NumberFormatObject>(ctx, receiver, "NumberFormat");
        if (!self) return Value();
        std::vector<Part> parts;
        if (!range_parts(ctx, *self, args, parts)) return Value();
        auto array = ObjectFactory::create_array(0);
        uint32_t index = 0;
        for (const Part& p : parts) {
            array->set_element(index++, Value(make_object({{"type", Value(p.type)}, {"value", Value(p.value)}, {"source", Value(p.source)}})));
        }
        return Value(array.release());
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        NumberFormatObject* self = unwrap(ctx, receiver);
        if (!self) return Value();
        const NumberFormatObject& nf = *self;
        const DigitOptions& d = nf.digits;
        auto result = ObjectFactory::create_object();
        auto number_value = [](int n) { return Value(static_cast<double>(n)); };
        put(result.get(), "locale", Value(nf.locale));
        put(result.get(), "numberingSystem", Value(nf.numbering_system));
        put(result.get(), "style", Value(nf.style));
        if (nf.style == "currency") {
            put(result.get(), "currency", Value(nf.currency));
            put(result.get(), "currencyDisplay", Value(nf.currency_display));
            put(result.get(), "currencySign", Value(nf.currency_sign));
        }
        if (nf.style == "unit") {
            put(result.get(), "unit", Value(nf.unit));
            put(result.get(), "unitDisplay", Value(nf.unit_display));
        }
        put(result.get(), "minimumIntegerDigits", number_value(d.minimum_integer_digits));
        if (d.has_fraction_slots()) {
            put(result.get(), "minimumFractionDigits", number_value(d.minimum_fraction_digits));
            put(result.get(), "maximumFractionDigits", number_value(d.maximum_fraction_digits));
        }
        if (d.has_significant_slots()) {
            put(result.get(), "minimumSignificantDigits", number_value(d.minimum_significant_digits));
            put(result.get(), "maximumSignificantDigits", number_value(d.maximum_significant_digits));
        }
        put(result.get(), "useGrouping", nf.use_grouping == "false" ? Value(false) : Value(nf.use_grouping));
        put(result.get(), "notation", Value(nf.notation));
        if (nf.notation == "compact") put(result.get(), "compactDisplay", Value(nf.compact_display));
        put(result.get(), "signDisplay", Value(nf.sign_display));
        put(result.get(), "roundingIncrement", number_value(d.rounding_increment));
        put(result.get(), "roundingMode", Value(d.rounding_mode));
        put(result.get(), "roundingPriority", Value(d.rounding_priority));
        put(result.get(), "trailingZeroDisplay", Value(d.trailing_zero_display));
        return Value(result.release());
    });
}

bool format_number_parts(Context& ctx, const std::string& locale, const std::vector<std::pair<std::string, Value>>& options,
                         const Numeric& value, std::vector<Part>& out) {
    Object* bag = make_object({});
    for (const auto& [name, v] : options) put(bag, name.c_str(), v);
    const Value arguments[2] = {Value(locale), Value(bag)};
    Value created = construct_number_format(ctx, arguments, Value(), true, Value());
    if (ctx.has_exception()) return false;
    NumberFormatObject* nf = DOMObject::Cast<NumberFormatObject>(created);
    if (!nf) return false;
    out = format_parts(*nf, value);
    return true;
}

bool number_to_locale_string(Context& ctx, const Value& number, const Value& locales, const Value& options, std::string& out) {
    const Value arguments[2] = {locales, options};
    Value created = construct_number_format(ctx, arguments, Value(), true, Value());
    if (ctx.has_exception()) return false;
    NumberFormatObject* nf = DOMObject::Cast<NumberFormatObject>(created);
    Numeric n;
    if (!nf || !to_intl_numeric(ctx, number, n)) return false;
    out = join(format_parts(*nf, n));
    return true;
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
