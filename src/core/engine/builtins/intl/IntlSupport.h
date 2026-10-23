/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

// What the Intl objects share: the abstract operations of ECMA-402 that read options and negotiate locales, and the
// helpers that build a constructor and its prototype. Compiled only with ICU4X.
#ifdef QUANTA_ICU4X

#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/builtins/intl/IntlICU.h"
#include "quanta/core/runtime/DOMObject.h"
#include "quanta/core/runtime/Object.h"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Quanta::IntlImpl {

using IntlICU::LocaleId;

// ---- The intrinsics of one realm -------------------------------------------------------------------------------

enum class Kind : uint8_t {
    Locale, Collator, NumberFormat, DateTimeFormat, PluralRules, ListFormat, RelativeTimeFormat, DisplayNames,
    Segmenter, DurationFormat, Segments, SegmentIterator, Count
};

// The prototype of `kind` for the realm `ctx` belongs to.
Object* prototype_of(Context& ctx, Kind kind);
// GetPrototypeFromConstructor(newTarget, "%Kind.prototype%"): newTarget's prototype when it is an object, else the
// intrinsic of newTarget's realm. Null with an exception pending.
Object* prototype_from_new_target(Context& ctx, const Value& new_target, Kind kind);

// ---- Errors ----------------------------------------------------------------------------------------------------

// Every helper below that can fail returns false (or an empty optional) with the exception pending on the context.
inline bool range_error(Context& ctx, const std::string& message) { ctx.throw_range_error(message); return false; }
inline bool type_error(Context& ctx, const std::string& message) { ctx.throw_type_error(message); return false; }

// ---- Options ---------------------------------------------------------------------------------------------------

// ToObject: null with a TypeError pending for undefined and null.
Object* to_object(Context& ctx, const Value& value);
// GetOptionsObject / CoerceOptionsToObject. Null with an exception pending (GetOptionsObject on a primitive).
Object* get_options_object(Context& ctx, const Value& options);
Object* coerce_options_to_object(Context& ctx, const Value& options);

// GetOption with type string. `allowed` empty means any string. Returns false with an exception pending; `out` is
// left empty when the option is absent.
bool get_string_option(Context& ctx, Object* options, const char* property, std::initializer_list<const char*> allowed,
                       std::optional<std::string>& out);
bool get_boolean_option(Context& ctx, Object* options, const char* property, std::optional<bool>& out);
// GetNumberOption: absent gives `fallback`; NaN or out of range is a RangeError.
bool get_number_option(Context& ctx, Object* options, const char* property, double minimum, double maximum,
                       std::optional<double> fallback, std::optional<double>& out);
// DefaultNumberOption for an already read value.
bool default_number_option(Context& ctx, const Value& value, double minimum, double maximum,
                           std::optional<double> fallback, std::optional<double>& out, const char* property);

// ---- Locales ---------------------------------------------------------------------------------------------------

// Whether `tag` is a structurally valid, canonicalizable language tag; the canonical form in `out`.
bool canonicalize_tag_or_throw(Context& ctx, std::string_view tag, std::string& out);
// CanonicalizeLocaleList.
bool canonicalize_locale_list(Context& ctx, const Value& locales, std::vector<std::string>& out);
// The tag held by an Intl.Locale object, or empty.
const std::string* locale_object_tag(const Value& value);

// Which of the keys of a locale's -u- extension a service understands, and the values it accepts for each.
struct ExtensionKey {
    const char* key;                               // "nu"
    // The values the service has for this key in the given available locale; the first is the default.
    std::vector<std::string> (*supported)(const std::string& data_locale);
};
// The result of ResolveLocale: the locale chosen, with only the supported -u- keywords it was asked for, and the
// value each relevant key resolved to.
struct Resolved {
    std::string locale;
    std::string data_locale;
    std::vector<std::pair<std::string, std::string>> values;
    const std::string* value_of(std::string_view key) const;
};

enum class Matcher : uint8_t { Lookup, BestFit };
bool get_matcher(Context& ctx, Object* options, Matcher& out);

// SupportedLocales(availableLocales, requestedLocales, options).
bool supported_locales(Context& ctx, const std::vector<std::string>& requested, const Value& options, Value& result);
// The Resolved for `requested` against the locales that have data. `overrides` carries the values the options object
// gave for relevant keys (key, value), which beat the ones in the tag.
Resolved resolve_locale(const std::vector<std::string>& requested, const std::vector<ExtensionKey>& keys,
                        const std::vector<std::pair<std::string, std::string>>& overrides, Matcher matcher);

// ---- Building objects ------------------------------------------------------------------------------------------

using NativeFn = std::function<Value(Context&, std::span<const Value>, Value)>;
using ConstructFn = std::function<Value(Context&, std::span<const Value>, Value, bool, Value)>;

// A method on `target`: writable, configurable, not enumerable; named and given the length.
void define_method(Object* target, const char* name, uint32_t length, NativeFn fn);
// An accessor "name" whose function is called "get name"; configurable, not enumerable.
void define_getter(Object* target, const char* name, NativeFn fn);
// Symbol.toStringTag: configurable only.
void define_to_string_tag(Object* target, const char* tag);
// Creates the constructor and its prototype object (constructor/prototype links in place); returns both.
struct Class {
    Function* constructor;
    Object* prototype;
};
Class define_class(Context& ctx, Object* intl, const char* name, uint32_t length, Kind kind, ConstructFn construct);

// Adds the static supportedLocalesOf to a service constructor.
void add_supported_locales_of(Context& ctx, Function* constructor);
// Whatever a constructor needs of its two arguments: the canonical requested locales and the options object.
bool read_locales_and_options(Context& ctx, std::span<const Value> args, std::vector<std::string>& requested, Object*& options);
// Walks an iterable the way a for-of loop does; `each` returns false (with an exception pending, if it is an error) to
// stop, which closes the iterator. False with an exception pending.
bool iterate(Context& ctx, const Value& iterable, const std::function<bool(const Value&)>& each);
// CreateDataProperty on a result object: never reaches a setter on the prototype chain.
void put(Object* object, const char* name, const Value& value);
// A plain object with the given properties (enumerable, writable, configurable), in order.
Object* make_object(std::initializer_list<std::pair<const char*, Value>> properties);

// The collation types the data has for a locale's language besides the default, sorted.
std::vector<std::string> collation_types(std::string_view tag);
// A function with no name that belongs to `owner` (a bound compare or format): the owner stays alive as long as the
// function does. The native body gets the owner as its fourth argument.
Value make_bound_function(Context& ctx, DOMObject* owner, uint32_t length,
                          std::function<Value(Context&, std::span<const Value>, DOMObject*)> body);

// The minor unit digits of a currency (ISO 4217 as CLDR has it).
int currency_digits(std::string_view code);
// The ten digits of a numbering system, or empty when it is not a numeric one; and the system whose digits begin with
// `zero` (UTF-8 of the digit zero), or empty.
std::string_view numbering_system_digits(std::string_view name);
std::string_view numbering_system_of_zero(std::string_view zero);
// The IANA name `name` stands for with the case the database gives it (any case matches), or empty when there is no
// such zone; links such as Asia/Calcutta are kept as they are. `link_target` is where a link points ("" otherwise).
std::string find_time_zone(std::string_view name, std::string* link_target = nullptr);
// IsWellFormedUnitIdentifier.
bool is_well_formed_unit(std::string_view unit);

// %Intl%.[[FallbackSymbol]]: where a legacy-constructed NumberFormat or DateTimeFormat keeps the real one.
Symbol* legacy_fallback_symbol(Context& ctx);

// The values supportedValuesOf lists for `key` ("calendar", "collation", "currency", "numberingSystem", "timeZone",
// "unit"); empty for any other key.
std::vector<std::string> supported_values(std::string_view key);

// Makes `value` an array of strings.
Value string_array(const std::vector<std::string>& items);

// The `this` of a method as the object of `T`, or a TypeError.
template <class T>
T* this_as(Context& ctx, const Value& receiver, const char* what) {
    T* object = DOMObject::Cast<T>(receiver);
    if (!object) { ctx.throw_type_error(std::string("Method called on an incompatible receiver, not an Intl.") + what); }
    return object;
}

// ---- The methods of other builtins that format through Intl --------------------------------------------------------
// Each returns false with an exception pending.

bool number_to_locale_string(Context& ctx, const Value& number, const Value& locales, const Value& options, std::string& out);
bool date_to_locale_string(Context& ctx, double time_value, const Value& locales, const Value& options, const char* required,
                           const char* defaults, std::string& out);
bool locale_compare(Context& ctx, const std::string& a, const std::string& b, const Value& locales, const Value& options, double& out);
bool locale_case(Context& ctx, const std::string& text, bool upper, const Value& locales, std::string& out);

// ---- Each service registers itself ---------------------------------------------------------------------------

void register_locale(Context& ctx, Object* intl);
void register_plural_rules(Context& ctx, Object* intl);
void register_list_format(Context& ctx, Object* intl);
void register_collator(Context& ctx, Object* intl);
void register_segmenter(Context& ctx, Object* intl);
void register_number_format(Context& ctx, Object* intl);
void register_relative_time_format(Context& ctx, Object* intl);
void register_display_names(Context& ctx, Object* intl);
void register_date_time_format(Context& ctx, Object* intl);
void register_duration_format(Context& ctx, Object* intl);

void register_all(Context& ctx, Object* intl);

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
