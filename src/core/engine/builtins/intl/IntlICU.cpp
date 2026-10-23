/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "quanta/core/engine/builtins/intl/IntlICU.h"

#include "icu4x/DataProvider.hpp"
#include "icu4x/LocaleFallbacker.hpp"
#include "icu4x/Locale.hpp"
#include "icu4x/LocaleCanonicalizer.hpp"

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern "C" {
bool quanta_icu_init(const uint8_t* data, size_t len);
intptr_t quanta_icu_format_percent(const char* locale, const char* decimal, uint8_t* out, size_t cap);
intptr_t quanta_icu_format_currency(const char* locale, const char* decimal, const char* code, uint8_t width,
                                    bool accounting, uint8_t* out, size_t cap);
intptr_t quanta_icu_format_relative(const char* locale, const char* decimal, uint8_t unit, uint8_t style, bool auto_words,
                                    uint8_t* out, size_t cap);
intptr_t quanta_icu_format_unit(const char* locale, const char* number, const char* decimal, const char* unit, uint8_t width,
                                uint8_t* out, size_t cap);
intptr_t quanta_icu_display_script(const char* locale, const char* script, bool short_form, uint8_t* out, size_t cap);
intptr_t quanta_icu_display_currency(const char* locale, const char* code, uint8_t* out, size_t cap);
intptr_t quanta_icu_format_datetime(const char* locale, const char* skeleton, int32_t year, uint8_t month, uint8_t day, uint8_t hour,
                                    uint8_t minute, uint8_t second, uint32_t nanosecond, const char* zone, int32_t offset_seconds,
                                    uint8_t* out, size_t cap);
intptr_t quanta_icu_format_compact(const char* locale, const char* decimal, bool long_form, uint8_t* out, size_t cap);
}

namespace Quanta::IntlICU {

namespace {

constexpr const char* kDataFileName = "quanta-icu.postcard";

const char* const kLocales[] = {
#include "IntlLocaleList.inc"
};

std::string executable_directory() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer, n);
    size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return ".";
    std::string path(buffer);
    size_t cut = path.find_last_of('/');
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
#else
    char buffer[4096];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0) return ".";
    std::string path(buffer, static_cast<size_t>(n));
    size_t cut = path.find_last_of('/');
    return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
#endif
}

std::vector<std::string> candidate_paths() {
    std::vector<std::string> paths;
    if (const char* env = std::getenv("QUANTA_ICU_DATA")) {
        if (*env) paths.emplace_back(env);
    }
    const std::string dir = executable_directory();
    paths.push_back(dir + "/" + kDataFileName);
    paths.push_back(dir + "/../" + kDataFileName);
    paths.push_back(dir + "/../share/quanta/" + kDataFileName);
    return paths;
}

struct Data {
    std::unique_ptr<icu4x::DataProvider> provider;
    std::unique_ptr<icu4x::LocaleCanonicalizer> canonicalizer;
    bool loaded = false;
    std::string searched;
};

Data& data() {
    static Data d;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::vector<std::string> paths = candidate_paths();
        for (size_t i = 0; i < paths.size(); i++) {
            if (i) d.searched += ", ";
            d.searched += paths[i];
        }
        for (const std::string& path : paths) {
            const uint8_t* bytes = nullptr;
            size_t size = 0;
#if defined(_WIN32)
            HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER length;
            GetFileSizeEx(file, &length);
            HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            CloseHandle(file);
            if (!mapping) continue;
            void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(mapping);
            if (!view) continue;
            bytes = static_cast<const uint8_t*>(view);
            size = static_cast<size_t>(length.QuadPart);
#else
            int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) continue;
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); continue; }
            void* view = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (view == MAP_FAILED) continue;
            bytes = static_cast<const uint8_t*>(view);
            size = static_cast<size_t>(st.st_size);
#endif
            // The mapping is kept for the life of the process: the provider reads from it whenever a formatter is made.
            auto provider = icu4x::DataProvider::from_byte_slice(icu4x::diplomat::span<const uint8_t>(bytes, size));
            if (!provider.is_ok()) continue;
            d.provider = std::move(provider).ok().value();
            auto canonicalizer = icu4x::LocaleCanonicalizer::create_extended_with_provider(*d.provider);
            if (canonicalizer.is_ok()) d.canonicalizer = std::move(canonicalizer).ok().value();
            // The blob keeps one copy of data that locales share (a tailoring that is the root's is left out), so a lookup
            // for a locale without its own entry has to walk up to its parent.
            auto fallbacker = icu4x::LocaleFallbacker::create_with_provider(*d.provider);
            if (fallbacker.is_ok()) d.provider->enable_locale_fallback_with(*std::move(fallbacker).ok().value());
            quanta_icu_init(bytes, size);
            d.loaded = true;
            break;
        }
    });
    return d;
}

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::vector<std::string_view> subtags_of(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t dash = s.find('-', start);
        if (dash == std::string_view::npos) dash = s.size();
        out.push_back(s.substr(start, dash - start));
        start = dash + 1;
    }
    return out;
}

}  // namespace

bool data_available() { return data().loaded; }

bool data_present() {
    static const bool present = [] {
        for (const std::string& path : candidate_paths()) {
#if defined(_WIN32)
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
#else
            if (access(path.c_str(), R_OK) == 0) return true;
#endif
        }
        return false;
    }();
    return present;
}

const icu4x::DataProvider& provider() { return *data().provider; }

std::string data_search_description() {
    data();
    return data().searched;
}

std::string LocaleId::base() const {
    std::string out = language;
    if (!script.empty()) out += "-" + script;
    if (!region.empty()) out += "-" + region;
    for (const auto& v : variants) out += "-" + v;
    return out;
}

std::string LocaleId::unicode_extension() const {
    if (attributes.empty() && keywords.empty()) return "";
    std::string out = "u";
    for (const auto& a : attributes) out += "-" + a;
    for (const auto& [key, value] : keywords) {
        out += "-" + key;
        if (!value.empty()) out += "-" + value;
    }
    return out;
}

namespace {

std::string transform_extension(const LocaleId& id) {
    if (id.transform_language.empty() && id.transform_fields.empty()) return "";
    std::string out = "t";
    if (!id.transform_language.empty()) out += "-" + id.transform_language;
    for (const auto& [key, value] : id.transform_fields) out += "-" + key + "-" + value;
    return out;
}

}  // namespace

std::string LocaleId::to_string() const {
    // Extensions are ordered by singleton: a < ... < t < u < ... < x.
    std::vector<std::pair<char, std::string>> parts;
    for (const auto& e : other_extensions) parts.emplace_back(e[0], e);
    if (std::string t = transform_extension(*this); !t.empty()) parts.emplace_back('t', t);
    if (std::string u = unicode_extension(); !u.empty()) parts.emplace_back('u', u);
    std::stable_sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out = base();
    for (const auto& [singleton, text] : parts) out += "-" + text;
    if (!private_use.empty()) out += "-" + private_use;
    return out;
}

const std::string* LocaleId::keyword(std::string_view key) const {
    for (const auto& kv : keywords) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool all_alpha(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (!is_alpha(c)) return false;
    return true;
}
bool all_digit(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (!is_digit(c)) return false;
    return true;
}
bool all_alnum(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (!is_alpha(c) && !is_digit(c)) return false;
    return true;
}

bool is_language_subtag(std::string_view s) {
    return all_alpha(s) && ((s.size() >= 2 && s.size() <= 3) || (s.size() >= 5 && s.size() <= 8));
}
bool is_script_subtag(std::string_view s) { return s.size() == 4 && all_alpha(s); }
bool is_region_subtag(std::string_view s) {
    return (s.size() == 2 && all_alpha(s)) || (s.size() == 3 && all_digit(s));
}
bool is_variant_subtag(std::string_view s) {
    if (!all_alnum(s)) return false;
    return (s.size() >= 5 && s.size() <= 8) || (s.size() == 4 && is_digit(s[0]));
}

struct AliasRow {
    const char* key;
    const char* from;
    const char* to;
};

#include "IntlLocaleData.inc"

template <size_t N>
const char* alias_of(const AliasRow (&rows)[N], std::string_view key, std::string_view from) {
    for (const AliasRow& row : rows) {
        if (key == row.key && from == row.from) return row.to;
    }
    return nullptr;
}

// language[-script][-region][-variants...] from `parts` starting at `i`; false if the first subtag is not a language.
bool parse_language_id(const std::vector<std::string_view>& parts, size_t& i, std::string& language, std::string& script,
                       std::string& region, std::vector<std::string>& variants) {
    if (i >= parts.size() || !is_language_subtag(parts[i])) return false;
    language = to_lower(parts[i++]);
    if (i < parts.size() && is_script_subtag(parts[i])) {
        script = to_lower(parts[i++]);
        script[0] = static_cast<char>(script[0] - 'a' + 'A');
    }
    if (i < parts.size() && is_region_subtag(parts[i])) {
        region = to_lower(parts[i++]);
        for (char& c : region) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    while (i < parts.size() && is_variant_subtag(parts[i])) {
        std::string variant = to_lower(parts[i++]);
        if (std::find(variants.begin(), variants.end(), variant) != variants.end()) return false;
        variants.push_back(std::move(variant));
    }
    std::sort(variants.begin(), variants.end());
    return true;
}

// The language aliases, replaced through ICU4X; a language of five or more letters has none and ICU4X does not take it.
std::string canonical_language_id(const std::string& base) {
    Data& d = data();
    if (!d.loaded || !d.canonicalizer) return base;
    std::unique_ptr<icu4x::Locale> locale = to_icu(base);
    if (!locale) return base;
    d.canonicalizer->canonicalize(*locale);
    return locale->to_string();
}

}  // namespace

std::optional<LocaleId> parse_tag(std::string_view tag) {
    if (tag.empty()) return std::nullopt;
    std::vector<std::string_view> parts = subtags_of(tag);
    for (std::string_view part : parts) {
        if (part.empty() || part.size() > 8 || !all_alnum(part)) return std::nullopt;
    }
    LocaleId id;
    size_t i = 0;
    if (!parse_language_id(parts, i, id.language, id.script, id.region, id.variants)) return std::nullopt;
    std::vector<char> seen_singletons;
    while (i < parts.size()) {
        if (parts[i].size() != 1) return std::nullopt;
        const char singleton = static_cast<char>(to_lower(parts[i])[0]);
        if (std::find(seen_singletons.begin(), seen_singletons.end(), singleton) != seen_singletons.end()) return std::nullopt;
        seen_singletons.push_back(singleton);
        i++;
        if (singleton == 'x') {
            if (i >= parts.size()) return std::nullopt;
            std::string text = "x";
            while (i < parts.size()) text += "-" + to_lower(parts[i++]);
            id.private_use = text;
            break;
        }
        size_t end = i;
        while (end < parts.size() && parts[end].size() > 1) end++;
        if (end == i) return std::nullopt;
        if (singleton == 'u') {
            size_t k = i;
            while (k < end && parts[k].size() > 2) id.attributes.push_back(to_lower(parts[k++]));
            while (k < end) {
                // key = alphanum alpha
                if (parts[k].size() != 2 || !is_alpha(parts[k][1])) return std::nullopt;
                std::string key = to_lower(parts[k++]);
                std::string value;
                while (k < end && parts[k].size() > 2) {
                    if (!value.empty()) value += "-";
                    value += to_lower(parts[k++]);
                }
                bool duplicate = false;
                for (const auto& kv : id.keywords) duplicate = duplicate || kv.first == key;
                if (!duplicate) id.keywords.emplace_back(std::move(key), std::move(value));
            }
            if (id.attributes.empty() && id.keywords.empty()) return std::nullopt;
        } else if (singleton == 't') {
            size_t k = i;
            // tlang, when the first subtag is not a field key (alpha digit).
            const bool field_key_first = parts[k].size() == 2 && is_alpha(parts[k][0]) && is_digit(parts[k][1]);
            if (!field_key_first) {
                std::string language, script, region;
                std::vector<std::string> variants;
                if (!parse_language_id(parts, k, language, script, region, variants) || k > end) return std::nullopt;
                std::string text = language;
                if (!script.empty()) text += "-" + script;
                if (!region.empty()) text += "-" + region;
                for (const auto& v : variants) text += "-" + v;
                id.transform_language = text;
            }
            while (k < end) {
                if (parts[k].size() != 2 || !is_alpha(parts[k][0]) || !is_digit(parts[k][1])) return std::nullopt;
                std::string key = to_lower(parts[k++]);
                std::string value;
                while (k < end && parts[k].size() >= 3) {
                    if (!value.empty()) value += "-";
                    value += to_lower(parts[k++]);
                }
                if (value.empty()) return std::nullopt;
                id.transform_fields.emplace_back(std::move(key), std::move(value));
            }
            if (id.transform_language.empty() && id.transform_fields.empty()) return std::nullopt;
        } else {
            std::string text(1, singleton);
            for (size_t k = i; k < end; k++) {
                if (parts[k].size() < 2) return std::nullopt;
                text += "-" + to_lower(parts[k]);
            }
            id.other_extensions.push_back(text);
        }
        i = end;
    }
    std::sort(id.attributes.begin(), id.attributes.end());
    id.attributes.erase(std::unique(id.attributes.begin(), id.attributes.end()), id.attributes.end());
    std::sort(id.keywords.begin(), id.keywords.end());
    std::sort(id.transform_fields.begin(), id.transform_fields.end());
    std::sort(id.other_extensions.begin(), id.other_extensions.end());
    return id;
}

LocaleId split_tag(std::string_view canonical) {
    std::optional<LocaleId> id = parse_tag(canonical);
    return id ? std::move(*id) : LocaleId{};
}

std::unique_ptr<icu4x::Locale> to_icu(std::string_view tag) {
    auto parsed = icu4x::Locale::from_string(tag);
    if (!parsed.is_ok()) return nullptr;
    return std::move(parsed).ok().value();
}

std::optional<std::string> canonicalize_tag(std::string_view tag) {
    std::optional<LocaleId> parsed = parse_tag(tag);
    if (!parsed) return std::nullopt;
    LocaleId id = std::move(*parsed);

    // The language aliases: through ICU4X, on the language identifier alone.
    auto replace_language_id = [](std::string& language, std::string& script, std::string& region,
                                  std::vector<std::string>& variants) {
        LocaleId base;
        base.language = language; base.script = script; base.region = region; base.variants = variants;
        const std::string canonical = canonical_language_id(base.base());
        if (std::optional<LocaleId> replaced = parse_tag(canonical)) {
            language = replaced->language; script = replaced->script; region = replaced->region;
            variants = replaced->variants;
        }
    };
    replace_language_id(id.language, id.script, id.region, id.variants);

    // Unicode extension: aliased types, "true" dropped.
    for (auto& [key, value] : id.keywords) {
        if (const char* preferred = alias_of(kUnicodeAliases, key, value)) value = preferred;
        if (value == "true") value.clear();
    }
    // Transformed extension: the language and the aliased field values.
    if (!id.transform_language.empty()) {
        if (std::optional<LocaleId> tlang = parse_tag(id.transform_language)) {
            replace_language_id(tlang->language, tlang->script, tlang->region, tlang->variants);
            std::string lowered = tlang->base();
            for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            id.transform_language = std::move(lowered);
        }
    }
    for (auto& [key, value] : id.transform_fields) {
        if (const char* preferred = alias_of(kTransformAliases, key, value)) value = preferred;
    }
    return id.to_string();
}

bool locale_listed(std::string_view base) {
    const char* const* begin = std::begin(kLocales);
    const char* const* end = std::end(kLocales);
    const char* const* it = std::lower_bound(begin, end, base, [](const char* a, std::string_view b) {
        return std::string_view(a) < b;
    });
    return it != end && std::string_view(*it) == base;
}

std::string best_available(std::string_view tag) {
    // BestAvailableLocale: drop trailing subtags until the rest is listed; an extension (and the singleton before it)
    // is not part of the locale being looked for.
    std::string candidate(tag);
    if (size_t ext = candidate.find("-u-"); ext != std::string::npos) candidate.resize(ext);
    if (size_t ext = candidate.find("-t-"); ext != std::string::npos) candidate.resize(ext);
    if (size_t ext = candidate.find("-x-"); ext != std::string::npos) candidate.resize(ext);
    while (!candidate.empty()) {
        if (candidate != "und" && locale_listed(candidate)) return candidate;
        size_t cut = candidate.find_last_of('-');
        if (cut == std::string::npos) break;
        // A single-character subtag left at the end is a singleton and goes with the subtag it introduced.
        if (cut >= 2 && candidate[cut - 2] == '-') cut -= 2;
        candidate.resize(cut);
    }
    return "";
}

std::string available_locale(std::string_view tag) {
    // Like best_available, but a region the data does not list on its own stays: CLDR's fallback chain gives en-US the
    // data of en, and the locale reported is still en-US.
    const std::string data = best_available(tag);
    if (data.empty()) return data;
    std::string candidate(tag);
    for (const char* marker : {"-u-", "-t-", "-x-"}) {
        if (size_t ext = candidate.find(marker); ext != std::string::npos) candidate.resize(ext);
    }
    if (candidate == data) return data;
    std::optional<LocaleId> id = parse_tag(candidate);
    if (!id || id->region.empty()) return data;
    LocaleId kept;
    kept.language = id->language;
    kept.script = id->script;
    kept.region = id->region;
    return best_available(kept.base()).empty() ? data : kept.base();
}

namespace shim {

namespace {

// Calls `call(buffer, capacity)` until the result fits; negative is a failure.
template <class Call>
std::optional<std::string> grow(Call call) {
    std::string out(256, '\0');
    intptr_t n = call(reinterpret_cast<uint8_t*>(out.data()), out.size());
    if (n < 0) return std::nullopt;
    if (static_cast<size_t>(n) > out.size()) {
        out.assign(static_cast<size_t>(n), '\0');
        n = call(reinterpret_cast<uint8_t*>(out.data()), out.size());
        if (n < 0) return std::nullopt;
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

}  // namespace

std::optional<std::string> percent(const std::string& locale, const std::string& decimal) {
    return grow([&](uint8_t* out, size_t cap) { return quanta_icu_format_percent(locale.c_str(), decimal.c_str(), out, cap); });
}

std::optional<std::string> currency(const std::string& locale, const std::string& decimal, const std::string& code,
                                    int width, bool accounting) {
    return grow([&](uint8_t* out, size_t cap) {
        return quanta_icu_format_currency(locale.c_str(), decimal.c_str(), code.c_str(), static_cast<uint8_t>(width), accounting, out, cap);
    });
}

std::optional<std::string> compact(const std::string& locale, const std::string& decimal, bool long_form) {
    return grow([&](uint8_t* out, size_t cap) { return quanta_icu_format_compact(locale.c_str(), decimal.c_str(), long_form, out, cap); });
}

std::optional<std::string> unit(const std::string& locale, const std::string& number, const std::string& decimal,
                                const std::string& unit, int width) {
    return grow([&](uint8_t* out, size_t cap) {
        return quanta_icu_format_unit(locale.c_str(), number.c_str(), decimal.c_str(), unit.c_str(), static_cast<uint8_t>(width), out, cap);
    });
}

std::optional<std::string> script_name(const std::string& locale, const std::string& script, bool short_form) {
    return grow([&](uint8_t* out, size_t cap) { return quanta_icu_display_script(locale.c_str(), script.c_str(), short_form, out, cap); });
}

std::optional<std::string> currency_name(const std::string& locale, const std::string& code) {
    return grow([&](uint8_t* out, size_t cap) { return quanta_icu_display_currency(locale.c_str(), code.c_str(), out, cap); });
}

std::optional<std::string> datetime(const std::string& locale, const std::string& skeleton, int year, int month, int day,
                                    int hour, int minute, int second, int nanosecond, const std::string& zone, int offset_seconds) {
    return grow([&](uint8_t* out, size_t cap) {
        return quanta_icu_format_datetime(locale.c_str(), skeleton.c_str(), year, static_cast<uint8_t>(month), static_cast<uint8_t>(day),
                                          static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(second),
                                          static_cast<uint32_t>(nanosecond), zone.c_str(), offset_seconds, out, cap);
    });
}

std::optional<std::string> relative_time(const std::string& locale, const std::string& decimal, int unit, int style, bool auto_words) {
    return grow([&](uint8_t* out, size_t cap) {
        return quanta_icu_format_relative(locale.c_str(), decimal.c_str(), static_cast<uint8_t>(unit), static_cast<uint8_t>(style), auto_words, out, cap);
    });
}

}  // namespace shim

const std::string& default_locale() {
    static const std::string locale = [] {
        std::string tag;
        for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
            const char* value = std::getenv(name);
            if (value && *value) { tag = value; break; }
        }
        if (size_t dot = tag.find('.'); dot != std::string::npos) tag.resize(dot);
        if (size_t at = tag.find('@'); at != std::string::npos) tag.resize(at);
        std::replace(tag.begin(), tag.end(), '_', '-');
        if (tag.empty() || tag == "C" || tag == "POSIX") tag = "en-US";
        std::optional<std::string> canonical = canonicalize_tag(tag);
        if (!canonical || best_available(*canonical).empty()) return std::string("en-US");
        return *canonical;
    }();
    return locale;
}

}  // namespace Quanta::IntlICU

#endif  // QUANTA_ICU4X
