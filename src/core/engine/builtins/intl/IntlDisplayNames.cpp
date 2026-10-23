/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "icu4x/DisplayNamesFallback.hpp"
#include "icu4x/DisplayNamesOptionsV1.hpp"
#include "icu4x/DisplayNamesStyle.hpp"
#include "icu4x/LanguageDisplay.hpp"
#include "icu4x/Locale.hpp"
#include "icu4x/LocaleDisplayNamesFormatter.hpp"
#include "icu4x/RegionDisplayNames.hpp"

#include <algorithm>
#include <cctype>

namespace Quanta::IntlImpl {

namespace {

struct DisplayNamesObject : DOMObject {
    std::string locale, icu_locale, style, type, fallback, language_display;
    std::unique_ptr<icu4x::LocaleDisplayNamesFormatter> locales;
    std::unique_ptr<icu4x::RegionDisplayNames> regions;
};

bool alpha_only(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
}

// English words for what the data has no names for; other languages get no name and fall back to the code.
const std::pair<const char*, const char*> kEnglishFields[] = {
    {"era", "era"}, {"year", "year"}, {"quarter", "quarter"}, {"month", "month"}, {"weekOfYear", "week"},
    {"weekday", "day of the week"}, {"day", "day"}, {"dayPeriod", "AM/PM"}, {"hour", "hour"}, {"minute", "minute"},
    {"second", "second"}, {"timeZoneName", "time zone"},
};
const std::pair<const char*, const char*> kEnglishCalendars[] = {
    {"buddhist", "Buddhist Calendar"}, {"chinese", "Chinese Calendar"}, {"coptic", "Coptic Calendar"},
    {"dangi", "Dangi Calendar"}, {"ethioaa", "Ethiopic Amete Alem Calendar"}, {"ethiopic", "Ethiopic Calendar"},
    {"gregory", "Gregorian Calendar"}, {"hebrew", "Hebrew Calendar"}, {"indian", "Indian National Calendar"},
    {"islamic", "Islamic Calendar"}, {"islamic-civil", "Islamic Calendar (tabular, civil epoch)"},
    {"islamic-tbla", "Islamic Calendar (tabular, astronomical epoch)"}, {"islamic-umalqura", "Islamic Calendar (Umm al-Qura)"},
    {"iso8601", "ISO-8601 Calendar"}, {"japanese", "Japanese Calendar"}, {"persian", "Persian Calendar"},
    {"roc", "Minguo Calendar"},
};

Value construct_display_names(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.DisplayNames requires 'new'"); return Value(); }
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::DisplayNames);
    if (ctx.has_exception()) return Value();
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
    const Value options_argument = args.size() > 1 ? args[1] : Value();
    if (options_argument.is_undefined()) { ctx.throw_type_error("Options are required"); return Value(); }
    Object* options = get_options_object(ctx, options_argument);
    if (!options) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> style, type, fallback, language_display;
    if (!get_string_option(ctx, options, "style", {"narrow", "short", "long"}, style)) return Value();
    if (!get_string_option(ctx, options, "type", {"language", "region", "script", "currency", "calendar", "dateTimeField"}, type)) return Value();
    if (!type) { ctx.throw_type_error("Option type is required"); return Value(); }
    if (!get_string_option(ctx, options, "fallback", {"code", "none"}, fallback)) return Value();
    if (!get_string_option(ctx, options, "languageDisplay", {"dialect", "standard"}, language_display)) return Value();

    Resolved resolved = resolve_locale(requested, {}, {}, matcher);
    auto* object = Heap::Allocate<DisplayNamesObject>();
    object->locale = resolved.locale;
    object->icu_locale = IntlICU::split_tag(resolved.locale).base();
    object->style = style ? *style : "long";
    object->type = *type;
    object->fallback = fallback ? *fallback : "code";
    object->language_display = language_display ? *language_display : "dialect";

    icu4x::DisplayNamesOptionsV1 icu_options;
    icu_options.style = object->style == "narrow" ? icu4x::DisplayNamesStyle::Narrow
        : object->style == "short" ? icu4x::DisplayNamesStyle::Short : icu4x::DisplayNamesStyle::Long;
    icu_options.fallback = object->fallback == "none" ? icu4x::DisplayNamesFallback::None : icu4x::DisplayNamesFallback::Code;
    icu_options.language_display = object->language_display == "standard" ? icu4x::LanguageDisplay::Standard : icu4x::LanguageDisplay::Dialect;
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(object->icu_locale);
    if (locale) {
        if (object->type == "language") {
            auto made = icu4x::LocaleDisplayNamesFormatter::create_v1_with_provider(IntlICU::provider(), *locale, icu_options);
            if (made.is_ok()) object->locales = std::move(made).ok().value();
        } else if (object->type == "region") {
            auto made = icu4x::RegionDisplayNames::create_v1_with_provider(IntlICU::provider(), *locale, icu_options);
            if (made.is_ok()) object->regions = std::move(made).ok().value();
        }
    }
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

}  // namespace

void register_display_names(Context& ctx, Object* intl) {
    Class names = define_class(ctx, intl, "DisplayNames", 2, Kind::DisplayNames, construct_display_names);
    add_supported_locales_of(ctx, names.constructor);
    Object* proto = names.prototype;

    define_method(proto, "of", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<DisplayNamesObject>(ctx, receiver, "DisplayNames");
        if (!self) return Value();
        std::string code;
        if (!(args.empty() ? Value() : args[0]).to_string_checked(ctx, code)) return Value();
        const std::string& type = self->type;
        std::string canonical;
        std::optional<std::string> name;
        if (type == "region") {
            if (!((code.size() == 2 && alpha_only(code)) || (code.size() == 3 && std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; })))) {
                return range_error(ctx, "Invalid region"), Value();
            }
            canonical = code;
            for (char& c : canonical) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (self->regions) {
                auto result = self->regions->of(canonical);
                if (result.is_ok()) name = std::move(result).ok().value();
            }
        } else if (type == "script") {
            if (code.size() != 4 || !alpha_only(code)) return range_error(ctx, "Invalid script"), Value();
            canonical = code;
            for (size_t i = 0; i < canonical.size(); i++) {
                canonical[i] = static_cast<char>(i == 0 ? std::toupper(static_cast<unsigned char>(canonical[i])) : std::tolower(static_cast<unsigned char>(canonical[i])));
            }
            name = IntlICU::shim::script_name(self->icu_locale, canonical, self->style != "long");
        } else if (type == "currency") {
            if (code.size() != 3 || !alpha_only(code)) return range_error(ctx, "Invalid currency"), Value();
            canonical = code;
            for (char& c : canonical) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            name = IntlICU::shim::currency_name(self->icu_locale, canonical);
        } else if (type == "calendar") {
            // type = alphanum{3,8} ("-" alphanum{3,8})*
            bool valid = !code.empty();
            size_t start = 0;
            while (valid) {
                size_t dash = code.find('-', start);
                size_t end = dash == std::string::npos ? code.size() : dash;
                if (end - start < 3 || end - start > 8) valid = false;
                for (size_t i = start; valid && i < end; i++) valid = std::isalnum(static_cast<unsigned char>(code[i])) != 0;
                if (dash == std::string::npos) break;
                start = dash + 1;
            }
            if (!valid) return range_error(ctx, "Invalid calendar"), Value();
            canonical = code;
            for (char& c : canonical) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (canonical == "gregorian") canonical = "gregory";
            else if (canonical == "ethiopic-amete-alem") canonical = "ethioaa";
            else if (canonical == "islamicc") canonical = "islamic-civil";
            if (IntlICU::split_tag(self->locale).language == "en") {
                for (const auto& [key, english] : kEnglishCalendars) {
                    if (canonical == key) name = english;
                }
            }
        } else if (type == "dateTimeField") {
            bool known = false;
            for (const auto& [key, english] : kEnglishFields) {
                if (code == key) {
                    known = true;
                    if (IntlICU::split_tag(self->locale).language == "en") name = english;
                }
            }
            if (!known) return range_error(ctx, "Invalid dateTimeField"), Value();
            canonical = code;
        } else {
            // language: a unicode_language_id and nothing after it
            std::optional<std::string> tag = IntlICU::canonicalize_tag(code);
            if (!tag || tag->find("-u-") != std::string::npos || tag->find("-t-") != std::string::npos ||
                tag->find("-x-") != std::string::npos || code.find('_') != std::string::npos ||
                (tag->size() > 1 && tag->find("-", 0) != std::string::npos && tag->compare(0, 2, "x-") == 0)) {
                return range_error(ctx, "Invalid language"), Value();
            }
            LocaleId id = IntlICU::split_tag(*tag);
            if (!id.keywords.empty() || !id.attributes.empty() || !id.other_extensions.empty() || !id.private_use.empty() ||
                !id.transform_language.empty() || !id.transform_fields.empty() || id.language.empty()) {
                return range_error(ctx, "Invalid language"), Value();
            }
            canonical = *tag;
            if (self->locales) {
                if (std::unique_ptr<icu4x::Locale> target = IntlICU::to_icu(canonical)) name = self->locales->of(*target);
            }
        }
        if (name && !name->empty() && *name != canonical) return Value(*name);
        if (name && !name->empty() && self->fallback == "none" && type == "language") return Value();
        if (self->fallback == "code") return Value(canonical);
        return Value();
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<DisplayNamesObject>(ctx, receiver, "DisplayNames");
        if (!self) return Value();
        Object* result = make_object({
            {"locale", Value(self->locale)}, {"style", Value(self->style)}, {"type", Value(self->type)},
            {"fallback", Value(self->fallback)},
        });
        if (self->type == "language") put(result, "languageDisplay", Value(self->language_display));
        return Value(result);
    });
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
