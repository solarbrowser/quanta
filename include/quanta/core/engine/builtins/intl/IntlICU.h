/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#ifdef QUANTA_ICU4X

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace icu4x {
class DataProvider;
class Locale;
}

namespace Quanta::IntlICU {

// The locale data Intl formats from: a blob built by `make icu-data`, found next to the executable (or where
// QUANTA_ICU_DATA says) and mapped when the first Intl object is made. False when there is none.
bool data_available();
// Whether a data file exists, without opening it: what decides at start-up whether Intl is the real one.
bool data_present();
// The provider over that blob. Only valid when data_available().
const icu4x::DataProvider& provider();
// Where the data was looked for, for the error a missing file gets.
std::string data_search_description();

// A language tag taken apart. The tag is in the form ICU4X prints: lower-case language, title-case script,
// upper-case region, variants sorted, extensions sorted by singleton, the private use extension last.
struct LocaleId {
    std::string language;                 // "en"; "und" when absent
    std::string script;                   // "Latn" or empty
    std::string region;                   // "US" or empty
    std::vector<std::string> variants;    // sorted
    std::vector<std::string> attributes;  // -u- attributes, sorted
    std::vector<std::pair<std::string, std::string>> keywords;  // -u- keywords sorted by key; value "" for "true"
    std::string transform_language;                             // the tlang of a -t- extension, or empty
    std::vector<std::pair<std::string, std::string>> transform_fields;  // -t- fields (key, value) sorted by key
    std::vector<std::string> other_extensions;                  // whole extensions of the other singletons
    std::string private_use;                                    // "x-..." or empty

    // The tag without extensions: language[-script][-region][-variants]
    std::string base() const;
    // The unicode extension alone ("u-ca-gregory"), or empty.
    std::string unicode_extension() const;
    std::string to_string() const;
    const std::string* keyword(std::string_view key) const;
};

// IsStructurallyValidLanguageTag + CanonicalizeUnicodeLocaleId: the canonical form of `tag`, or nullopt when it is
// not a valid tag. The language aliases need the data; without it only the syntax is checked and the case fixed.
std::optional<std::string> canonicalize_tag(std::string_view tag);
// A tag taken apart without any canonicalization beyond case; nullopt when it is not structurally valid.
std::optional<LocaleId> parse_tag(std::string_view tag);
// Takes a canonical tag apart (the tag must be valid).
LocaleId split_tag(std::string_view canonical);

// A parsed ICU4X locale for a canonical tag (nullptr when it does not parse).
std::unique_ptr<icu4x::Locale> to_icu(std::string_view tag);

// Whether the data has an entry for exactly this locale (no extensions).
bool locale_listed(std::string_view base);
// BestAvailableLocale: the longest prefix of `tag` (extensions dropped) that has data, or empty.
std::string best_available(std::string_view tag);

// AvailableLocale: what Intl reports for `tag`, which keeps a region the data only reaches through fallback (en-US).
// The data itself is read from best_available(). Empty when no prefix has data.
std::string available_locale(std::string_view tag);

// The formatters ICU4X's C API does not have, from the Rust shim in third_party/quanta_icu. Each takes the locale (with
// its -u-nu- keyword) and the number as text, and gives the formatted string, or nullopt when there is no data for it.
namespace shim {
std::optional<std::string> percent(const std::string& locale, const std::string& decimal);
// width: 0 symbol, 1 narrow symbol, 2 code, 3 name
std::optional<std::string> currency(const std::string& locale, const std::string& decimal, const std::string& code,
                                    int width, bool accounting);
std::optional<std::string> compact(const std::string& locale, const std::string& decimal, bool long_form);
// A CLDR simple unit ("kilometer", or "kilometer-per-hour") written around an already formatted number; `decimal` is the
// same value, which picks the plural form. width: 0 long, 1 short, 2 narrow. Only the units the data has names for.
std::optional<std::string> unit(const std::string& locale, const std::string& number, const std::string& decimal,
                                const std::string& unit, int width);
std::optional<std::string> script_name(const std::string& locale, const std::string& script, bool short_form);
std::optional<std::string> currency_name(const std::string& locale, const std::string& code);
// A date and time written from a UTS 35 skeleton. The result is the text, a NUL, and the parts as "name,start,end;" with
// byte offsets into the text. `zone` is an IANA id (or empty) and `offset_seconds` the offset from UTC at that moment.
std::optional<std::string> datetime(const std::string& locale, const std::string& skeleton, int year, int month, int day,
                                    int hour, int minute, int second, int nanosecond, const std::string& zone, int offset_seconds);
// unit: 0 second .. 7 year; style: 0 long, 1 short, 2 narrow; `decimal` is signed (negative is the past)
std::optional<std::string> relative_time(const std::string& locale, const std::string& decimal, int unit, int style, bool auto_words);
}  // namespace shim

// The locale a program that names none gets: DefaultLocale of the host. "en-US" unless the environment says more.
const std::string& default_locale();

}  // namespace Quanta::IntlICU

#endif  // QUANTA_ICU4X
