/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "icu4x/Locale.hpp"
#include "icu4x/LocaleExpander.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace Quanta::IntlImpl {

namespace {

struct LocaleObject : DOMObject {
    std::string tag;   // canonical
    explicit LocaleObject(std::string t) : tag(std::move(t)) {}
};

bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

bool all_of(std::string_view s, bool (*pred)(char)) {
    return !s.empty() && std::all_of(s.begin(), s.end(), pred);
}

bool is_language_subtag(std::string_view s) {
    return all_of(s, is_alpha) && ((s.size() >= 2 && s.size() <= 3) || (s.size() >= 5 && s.size() <= 8));
}
bool is_script_subtag(std::string_view s) { return s.size() == 4 && all_of(s, is_alpha); }
bool is_region_subtag(std::string_view s) {
    return (s.size() == 2 && all_of(s, is_alpha)) || (s.size() == 3 && all_of(s, is_digit));
}
bool is_variant_subtag(std::string_view s) {
    if (!all_of(s, is_alnum)) return false;
    return (s.size() >= 5 && s.size() <= 8) || (s.size() == 4 && is_digit(s[0]));
}

std::vector<std::string_view> split_dash(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t dash = s.find('-', start);
        if (dash == std::string_view::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, dash - start));
        start = dash + 1;
    }
    return out;
}

// type = alphanum{3,8} ("-" alphanum{3,8})*
bool is_unicode_type(std::string_view s) {
    if (s.empty()) return false;
    for (std::string_view part : split_dash(s)) {
        if (part.size() < 3 || part.size() > 8 || !all_of(part, is_alnum)) return false;
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

LocaleObject* locale_of(Context& ctx, const Value& receiver) {
    return this_as<LocaleObject>(ctx, receiver, "Locale");
}

std::optional<std::string> keyword_of(const LocaleId& id, const char* key) {
    if (const std::string* v = id.keyword(key)) return *v;
    return std::nullopt;
}

Value optional_string(const std::optional<std::string>& s) { return s ? Value(*s) : Value(); }
Value nonempty_string(const std::string& s) { return s.empty() ? Value() : Value(s); }

// ---- Construction -------------------------------------------------------------------------------------------

struct TagOptions {
    std::optional<std::string> language, script, region, variants;
    std::optional<std::string> calendar, collation, first_day_of_week, hour_cycle, case_first, numeric, numbering_system;
};

// ApplyOptionsToTag and ApplyUnicodeExtensionToTag: the canonical form of `tag` once the options have had their say.
bool apply_options(Context& ctx, const std::string& tag, const TagOptions& o, std::string& out) {
    std::optional<std::string> canonical = IntlICU::canonicalize_tag(tag);
    if (!canonical) return range_error(ctx, "Incorrect locale information provided");
    LocaleId id = IntlICU::split_tag(*canonical);
    if (o.language) id.language = lower(*o.language);
    if (o.script) {
        std::string s = lower(*o.script);
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        id.script = s;
    }
    if (o.region) {
        std::string r = *o.region;
        for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        id.region = r;
    }
    if (o.variants) {
        id.variants.clear();
        for (std::string_view v : split_dash(*o.variants)) id.variants.push_back(lower(v));
        std::sort(id.variants.begin(), id.variants.end());
    }
    auto set_keyword = [&](const char* key, const std::optional<std::string>& value) {
        if (!value) return;
        std::string v = lower(*value);
        if (v == "true") v.clear();
        for (auto& kv : id.keywords) {
            if (kv.first == key) { kv.second = v; return; }
        }
        id.keywords.emplace_back(key, v);
    };
    set_keyword("ca", o.calendar);
    set_keyword("co", o.collation);
    set_keyword("fw", o.first_day_of_week);
    set_keyword("hc", o.hour_cycle);
    set_keyword("kf", o.case_first);
    set_keyword("kn", o.numeric);
    set_keyword("nu", o.numbering_system);
    std::sort(id.keywords.begin(), id.keywords.end());
    std::optional<std::string> result = IntlICU::canonicalize_tag(id.to_string());
    if (!result) return range_error(ctx, "Incorrect locale information provided");
    out = std::move(*result);
    return true;
}

// The options in the order the specification reads them.
bool read_options(Context& ctx, Object* options, TagOptions& o) {
    auto text = [&](const char* name, std::optional<std::string>& out, bool (*valid)(std::string_view)) {
        if (!get_string_option(ctx, options, name, {}, out)) return false;
        if (out && !valid(*out)) return range_error(ctx, std::string("Invalid value for option ") + name);
        return true;
    };
    if (!text("language", o.language, is_language_subtag)) return false;
    if (!text("script", o.script, is_script_subtag)) return false;
    if (!text("region", o.region, is_region_subtag)) return false;
    if (!text("variants", o.variants, [](std::string_view v) {
            if (v.empty()) return false;
            std::vector<std::string> seen;
            for (std::string_view part : split_dash(v)) {
                if (!is_variant_subtag(part)) return false;
                std::string l = lower(part);
                if (std::find(seen.begin(), seen.end(), l) != seen.end()) return false;
                seen.push_back(std::move(l));
            }
            return true;
        })) return false;
    if (!text("calendar", o.calendar, is_unicode_type)) return false;
    if (!text("collation", o.collation, is_unicode_type)) return false;
    if (!get_string_option(ctx, options, "firstDayOfWeek", {}, o.first_day_of_week)) return false;
    if (o.first_day_of_week) {
        std::string& fw = *o.first_day_of_week;
        static const char* const kDays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun"};
        if (fw.size() == 1 && fw[0] >= '0' && fw[0] <= '7') fw = kDays[fw[0] - '0'];
        if (!is_unicode_type(fw)) return range_error(ctx, "Invalid value for option firstDayOfWeek");
    }
    if (!get_string_option(ctx, options, "hourCycle", {"h11", "h12", "h23", "h24"}, o.hour_cycle)) return false;
    if (!get_string_option(ctx, options, "caseFirst", {"upper", "lower", "false"}, o.case_first)) return false;
    std::optional<bool> numeric;
    if (!get_boolean_option(ctx, options, "numeric", numeric)) return false;
    if (numeric) o.numeric = *numeric ? "true" : "false";
    if (!text("numberingSystem", o.numbering_system, is_unicode_type)) return false;
    return true;
}

Value construct_locale(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.Locale requires 'new'"); return Value(); }
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::Locale);
    if (ctx.has_exception()) return Value();
    const Value tag_argument = args.empty() ? Value() : args[0];
    const Value options_argument = args.size() > 1 ? args[1] : Value();
    if (!tag_argument.is_string() && !tag_argument.is_object() && !tag_argument.is_function()) {
        ctx.throw_type_error("First argument to Intl.Locale constructor can't be empty or missing");
        return Value();
    }
    std::string tag;
    if (const std::string* held = locale_object_tag(tag_argument)) {
        tag = *held;
    } else if (!tag_argument.to_string_checked(ctx, tag)) {
        return Value();
    }
    Object* options = coerce_options_to_object(ctx, options_argument);
    if (!options) return Value();
    if (!IntlICU::canonicalize_tag(tag)) { ctx.throw_range_error("Incorrect locale information provided"); return Value(); }
    TagOptions parsed;
    if (!read_options(ctx, options, parsed)) return Value();
    std::string result;
    if (!apply_options(ctx, tag, parsed, result)) return Value();
    LocaleObject* locale = Heap::Allocate<LocaleObject>(std::move(result));
    if (proto) locale->initialize_prototype(proto);
    return Value(static_cast<Object*>(locale));
}

// ---- maximize / minimize ------------------------------------------------------------------------------------

bool expand(Context& ctx, const std::string& tag, bool maximize, std::string& out) {
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(tag);
    if (!locale) { out = tag; return true; }
    auto expander = icu4x::LocaleExpander::create_extended_with_provider(IntlICU::provider());
    if (!expander.is_ok()) { out = tag; return true; }
    auto instance = std::move(expander).ok().value();
    instance->maximize(*locale);
    // Where the data has no likely language (a bare "und", or a macro region such as 150) English stands in for it.
    if (locale->language() == "und" && !locale->script()) {
        locale->set_language("en");
        instance->maximize(*locale);
    }
    if (!maximize) instance->minimize(*locale);
    out = locale->to_string();
    return true;
}

// ---- Locale information -------------------------------------------------------------------------------------

struct RegionRow {
    const char* region;
    const char* values;   // space separated
};
struct WeekRow {
    const char* region;
    int8_t first_day, minimal_days, weekend_start, weekend_end;   // 0: the world's value applies
};
#include "IntlLocaleInfo.inc"

template <size_t N>
const char* find_row(const RegionRow (&rows)[N], std::string_view region) {
    const RegionRow* end = rows + N;
    const RegionRow* it = std::lower_bound(rows, end, region, [](const RegionRow& row, std::string_view key) {
        return std::string_view(row.region) < key;
    });
    return it != end && region == it->region ? it->values : nullptr;
}

std::vector<std::string> split_spaces(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t space = s.find(' ', start);
        if (space == std::string_view::npos) space = s.size();
        out.emplace_back(s.substr(start, space - start));
        start = space + 1;
    }
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// RegionPreference: the region whose data a locale reads, from the strongest signal it carries: the "rg" override (when
// `known` has data for it), the region subtag, the "sd" subdivision, the likely region, then the world.
std::string region_preference(const LocaleId& id, bool (*known)(std::string_view)) {
    if (const std::string* rg = id.keyword("rg"); rg && rg->size() >= 2) {
        std::string region = upper(rg->substr(0, 2));
        if (known(region)) return region;
    }
    if (!id.region.empty()) return id.region;
    if (const std::string* sd = id.keyword("sd"); sd && sd->size() >= 2) return upper(sd->substr(0, 2));
    if (id.language != "und" || !id.script.empty()) {
        LocaleId base = IntlICU::split_tag(id.base());
        base.variants.clear();
        std::string tag;
        // The likely region needs the data; without it the world is as good a guess as any.
        if (std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(base.to_string())) {
            auto expander = icu4x::LocaleExpander::create_extended_with_provider(IntlICU::provider());
            if (expander.is_ok()) {
                std::move(expander).ok().value()->maximize(*locale);
                if (auto region = locale->region()) return *region;
            }
        }
    }
    return "001";
}

bool has_calendars(std::string_view region) { return find_row(kCalendarPreferences, region) != nullptr; }
bool has_hours(std::string_view region) { return find_row(kHourPreferences, region) != nullptr; }
bool has_week(std::string_view region) {
    const WeekRow* end = kWeekData + std::size(kWeekData);
    const WeekRow* it = std::lower_bound(kWeekData, end, region, [](const WeekRow& row, std::string_view key) {
        return std::string_view(row.region) < key;
    });
    return it != end && region == it->region;
}

const WeekRow& week_row(std::string_view region) {
    const WeekRow* end = kWeekData + std::size(kWeekData);
    const WeekRow* it = std::lower_bound(kWeekData, end, region, [](const WeekRow& row, std::string_view key) {
        return std::string_view(row.region) < key;
    });
    if (it != end && region == it->region) return *it;
    return kWeekData[0];   // "001" sorts first
}

std::vector<std::string> calendars_of(const LocaleId& id) {
    if (const std::string* ca = id.keyword("ca"); ca && !ca->empty()) return {*ca};
    std::string region = region_preference(id, has_calendars);
    const char* values = find_row(kCalendarPreferences, region);
    return split_spaces(values ? values : find_row(kCalendarPreferences, "001"));
}

std::vector<std::string> hour_cycles_of(const LocaleId& id) {
    if (const std::string* hc = id.keyword("hc"); hc && !hc->empty()) return {*hc};
    std::string region = region_preference(id, has_hours);
    const char* values = find_row(kHourPreferences, id.language + "-" + region);
    if (!values) values = find_row(kHourPreferences, region);
    if (!values) values = find_row(kHourPreferences, "001");
    return split_spaces(values);
}

std::vector<std::string> collations_of(const LocaleId& id) {
    if (const std::string* co = id.keyword("co"); co && !co->empty()) return {*co};
    struct Tailoring { const char* language; const char* types; };
    static const Tailoring tailorings[] = {
        {"ar", "compat"}, {"cs", "trad"}, {"da", "trad"}, {"de", "phonebk"}, {"es", "trad"}, {"fi", "trad"},
        {"hi", "direct"}, {"ja", "unihan"}, {"ko", "searchjl unihan"}, {"sv", "trad"}, {"zh", "big5han gb2312 pinyin stroke unihan zhuyin"},
    };
    std::vector<std::string> out = {"emoji", "eor"};
    for (const Tailoring& t : tailorings) {
        if (id.language == t.language) {
            for (std::string& type : split_spaces(t.types)) out.push_back(std::move(type));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> numbering_systems_of(const LocaleId& id) {
    if (const std::string* nu = id.keyword("nu"); nu && !nu->empty()) return {*nu};
    static const std::pair<const char*, const char*> defaults[] = {
        {"ar", "arab"}, {"bn", "beng"}, {"fa", "arabext"}, {"mr", "deva"}, {"my", "mymr"}, {"ne", "deva"}, {"ps", "arabext"},
    };
    for (const auto& [language, system] : defaults) {
        if (id.language == language) return {system};
    }
    return {"latn"};
}

bool right_to_left(const LocaleId& id) {
    std::string script = id.script;
    if (script.empty()) {
        LocaleId base = IntlICU::split_tag(id.base());
        base.variants.clear();
        if (std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(base.to_string())) {
            auto expander = icu4x::LocaleExpander::create_extended_with_provider(IntlICU::provider());
            if (expander.is_ok()) {
                std::move(expander).ok().value()->maximize(*locale);
                if (auto found = locale->script()) script = *found;
            }
        }
    }
    static const char* const rtl[] = {"Adlm", "Arab", "Hebr", "Mand", "Nkoo", "Rohg", "Samr", "Syrc", "Thaa"};
    return std::find(std::begin(rtl), std::end(rtl), script) != std::end(rtl);
}

Value week_info_of(const LocaleId& id) {
    const WeekRow& row = week_row(region_preference(id, has_week));
    const WeekRow& world = kWeekData[0];
    int first = row.first_day ? row.first_day : world.first_day;
    if (const std::string* fw = id.keyword("fw")) {
        static const char* const days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
        for (int i = 0; i < 7; i++) {
            if (*fw == days[i]) first = i + 1;
        }
    }
    int start = row.weekend_start ? row.weekend_start : world.weekend_start;
    int end = row.weekend_end ? row.weekend_end : world.weekend_end;
    auto weekend = ObjectFactory::create_array(0);
    uint32_t count = 0;
    for (int day = start;; day = day % 7 + 1) {
        weekend->set_element(count++, Value(static_cast<double>(day)));
        if (day == end) break;
    }
    return Value(make_object({
        {"firstDay", Value(static_cast<double>(first))},
        {"weekend", Value(weekend.release())},
    }));
}

}  // namespace

std::vector<std::string> collation_types(std::string_view tag) {
    std::vector<std::string> types = collations_of(IntlICU::split_tag(IntlICU::best_available(tag).empty() ? "und" : IntlICU::best_available(tag)));
    return types;
}

// The sorted, duplicate-free values the Intl services accept for a key of supportedValuesOf; empty for an unknown key.
std::vector<std::string> supported_values(std::string_view key) {
    std::vector<std::string> out;
    if (key == "calendar") {
        out = split_spaces("buddhist chinese coptic dangi ethioaa ethiopic gregory hebrew indian islamic-civil "
                           "islamic-tbla islamic-umalqura iso8601 japanese persian roc");
    } else if (key == "collation") {
        out = {"big5han", "compat", "direct", "emoji", "eor", "gb2312", "phonebk", "pinyin", "searchjl", "stroke",
               "trad", "unihan", "zhuyin"};
    } else if (key == "currency") {
        out = split_spaces(kCurrencies);
    } else if (key == "numberingSystem") {
        out = split_spaces(kNumberingSystems);
    } else if (key == "timeZone") {
        for (const RegionRow& row : kZoneTab) {
            for (std::string& zone : split_spaces(row.values)) out.push_back(std::move(zone));
        }
        out.push_back("UTC");
        for (int hours = 1; hours <= 14; hours++) out.push_back("Etc/GMT-" + std::to_string(hours));
        for (int hours = 1; hours <= 12; hours++) out.push_back("Etc/GMT+" + std::to_string(hours));
    } else if (key == "unit") {
        out = split_spaces("acre bit byte celsius centimeter day degree fahrenheit fluid-ounce foot gallon gigabit "
                           "gigabyte gram hectare hour inch kilobit kilobyte kilogram kilometer liter megabit megabyte "
                           "meter microsecond mile mile-scandinavian milliliter millimeter millisecond minute month "
                           "nanosecond ounce percent petabyte pound second stone terabit terabyte week yard year");
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int currency_digits(std::string_view code) {
    const char* digits = find_row(kCurrencyDigits, code);
    return digits ? digits[0] - '0' : kDefaultCurrencyDigits;
}

std::string_view numbering_system_digits(std::string_view name) {
    const char* digits = find_row(kNumberingDigits, name);
    return digits ? digits : "";
}

std::string_view numbering_system_of_zero(std::string_view zero) {
    for (const RegionRow& row : kNumberingDigits) {
        if (std::string_view(row.values).substr(0, zero.size()) == zero) return row.region;
    }
    return "";
}

std::string find_time_zone(std::string_view name, std::string* link_target) {
    auto same = [&](const char* candidate) {
        const std::string_view c = candidate;
        if (c.size() != name.size()) return false;
        for (size_t i = 0; i < c.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(c[i])) != std::tolower(static_cast<unsigned char>(name[i]))) return false;
        }
        return true;
    };
    if (link_target) link_target->clear();
    for (const char* utc : {"UTC", "Etc/UTC", "Etc/UCT", "UCT", "Etc/Universal", "Universal", "Etc/Zulu", "Zulu", "GMT", "Etc/GMT", "Etc/Greenwich", "Greenwich", "GMT0", "Etc/GMT0", "GMT+0", "GMT-0", "Etc/GMT+0", "Etc/GMT-0"}) {
        if (same(utc)) return utc;
    }
    for (int hours = 1; hours <= 14; hours++) {
        for (const char* sign : {"+", "-"}) {
            const std::string etc = std::string("Etc/GMT") + sign + std::to_string(hours);
            if (same(etc.c_str())) return etc;
        }
    }
    if (same("Etc/GMT")) return "Etc/GMT";
    for (const RegionRow& row : kZoneTab) {
        std::string_view zones = row.values;
        size_t start = 0;
        while (start < zones.size()) {
            size_t space = zones.find(' ', start);
            if (space == std::string_view::npos) space = zones.size();
            const std::string zone(zones.substr(start, space - start));
            if (same(zone.c_str())) return zone;
            start = space + 1;
        }
    }
    {
        std::string_view all = kZoneNames;
        size_t start = 0;
        while (start < all.size()) {
            size_t space = all.find(' ', start);
            if (space == std::string_view::npos) space = all.size();
            const std::string zone(all.substr(start, space - start));
            if (same(zone.c_str())) return zone;
            start = space + 1;
        }
    }
    for (const RegionRow& row : kZoneLinks) {
        if (same(row.region)) {
            if (link_target) *link_target = row.values;
            return row.region;
        }
    }
    return "";
}

bool is_well_formed_unit(std::string_view unit) {
    const std::vector<std::string> sanctioned = supported_values("unit");
    auto simple = [&](std::string_view u) { return std::find(sanctioned.begin(), sanctioned.end(), u) != sanctioned.end(); };
    if (simple(unit)) return true;
    const size_t per = unit.find("-per-");
    return per != std::string_view::npos && simple(unit.substr(0, per)) && simple(unit.substr(per + 5));
}

const std::string* locale_object_tag(const Value& value) {
    if (LocaleObject* locale = DOMObject::Cast<LocaleObject>(value)) return &locale->tag;
    return nullptr;
}

void register_locale(Context& ctx, Object* intl) {
    Class locale = define_class(ctx, intl, "Locale", 1, Kind::Locale, construct_locale);
    Object* proto = locale.prototype;

    define_method(proto, "maximize", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        if (!self) return Value();
        std::string tag;
        if (!expand(ctx, self->tag, true, tag)) return Value();
        LocaleObject* result = Heap::Allocate<LocaleObject>(std::move(tag));
        if (Object* p = prototype_of(ctx, Kind::Locale)) result->initialize_prototype(p);
        return Value(static_cast<Object*>(result));
    });
    define_method(proto, "minimize", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        if (!self) return Value();
        std::string tag;
        if (!expand(ctx, self->tag, false, tag)) return Value();
        LocaleObject* result = Heap::Allocate<LocaleObject>(std::move(tag));
        if (Object* p = prototype_of(ctx, Kind::Locale)) result->initialize_prototype(p);
        return Value(static_cast<Object*>(result));
    });
    define_method(proto, "toString", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        return self ? Value(self->tag) : Value();
    });

    struct Info {
        const char* name;
        std::vector<std::string> (*list)(const LocaleId&);
    };
    static const Info lists[] = {
        {"getCalendars", calendars_of}, {"getCollations", collations_of}, {"getHourCycles", hour_cycles_of},
        {"getNumberingSystems", numbering_systems_of},
    };
    for (const Info& info : lists) {
        const auto list = info.list;
        define_method(proto, info.name, 0, [list](Context& ctx, std::span<const Value>, Value receiver) -> Value {
            LocaleObject* self = locale_of(ctx, receiver);
            if (!self) return Value();
            return string_array(list(IntlICU::split_tag(self->tag)));
        });
    }
    define_method(proto, "getTimeZones", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        if (!self) return Value();
        LocaleId id = IntlICU::split_tag(self->tag);
        if (id.region.empty()) return Value();
        const char* zones = find_row(kZoneTab, id.region);
        return string_array(zones ? split_spaces(zones) : std::vector<std::string>());
    });
    define_method(proto, "getTextInfo", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        if (!self) return Value();
        return Value(make_object({{"direction", Value(right_to_left(IntlICU::split_tag(self->tag)) ? "rtl" : "ltr")}}));
    });
    define_method(proto, "getWeekInfo", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        LocaleObject* self = locale_of(ctx, receiver);
        if (!self) return Value();
        return week_info_of(IntlICU::split_tag(self->tag));
    });

    struct Getter {
        const char* name;
        Value (*read)(const LocaleId&);
    };
    static const Getter getters[] = {
        {"baseName", [](const LocaleId& id) { return Value(id.base()); }},
        {"calendar", [](const LocaleId& id) { return optional_string(keyword_of(id, "ca")); }},
        {"caseFirst", [](const LocaleId& id) { return optional_string(keyword_of(id, "kf")); }},
        {"collation", [](const LocaleId& id) { return optional_string(keyword_of(id, "co")); }},
        {"firstDayOfWeek", [](const LocaleId& id) { return optional_string(keyword_of(id, "fw")); }},
        {"hourCycle", [](const LocaleId& id) { return optional_string(keyword_of(id, "hc")); }},
        {"language", [](const LocaleId& id) { return Value(id.language); }},
        {"numberingSystem", [](const LocaleId& id) { return optional_string(keyword_of(id, "nu")); }},
        {"numeric", [](const LocaleId& id) {
             std::optional<std::string> v = keyword_of(id, "kn");
             return Value(v && (v->empty() || *v == "true"));
         }},
        {"region", [](const LocaleId& id) { return nonempty_string(id.region); }},
        {"script", [](const LocaleId& id) { return nonempty_string(id.script); }},
        {"variants", [](const LocaleId& id) {
             std::string joined;
             for (const auto& v : id.variants) joined += (joined.empty() ? "" : "-") + v;
             return nonempty_string(joined);
         }},
    };
    for (const Getter& g : getters) {
        const auto read = g.read;
        define_getter(proto, g.name, [read](Context& ctx, std::span<const Value>, Value receiver) -> Value {
            LocaleObject* self = locale_of(ctx, receiver);
            if (!self) return Value();
            return read(IntlICU::split_tag(self->tag));
        });
    }
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
