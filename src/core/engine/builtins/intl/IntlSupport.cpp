/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Realm.h"
#include "quanta/core/runtime/Symbol.h"

#include <algorithm>
#include <cmath>

namespace Quanta::IntlImpl {

// ---- The intrinsics of one realm -------------------------------------------------------------------------------

static_assert(static_cast<size_t>(Kind::Count) <= Realm::kIntlKinds);

Object* prototype_of(Context& ctx, Kind kind) {
    Realm* realm = ctx.realm();
    return realm ? realm->intl_prototypes[static_cast<size_t>(kind)] : nullptr;
}

Object* prototype_from_new_target(Context& ctx, const Value& new_target, Kind kind) {
    Object* nt = new_target.is_function() ? static_cast<Object*>(new_target.as_function())
                                          : new_target.as_object_or_null();
    if (!nt) return prototype_of(ctx, kind);
    Value proto = nt->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    if (proto.is_function()) return static_cast<Object*>(proto.as_function());
    if (proto.is_object()) return proto.as_object();
    // newTarget's own realm supplies the intrinsic when it is not this one.
    if (Context* owner = Engine::find_realm_owning_function(nt)) {
        if (Realm* realm = owner->realm()) {
            if (Object* intrinsic = realm->intl_prototypes[static_cast<size_t>(kind)]) return intrinsic;
        }
    }
    return prototype_of(ctx, kind);
}

// ---- Options ---------------------------------------------------------------------------------------------------

Object* to_object(Context& ctx, const Value& value) {
    if (value.is_nullish()) { ctx.throw_type_error("Cannot convert undefined or null to an object"); return nullptr; }
    if (value.is_function()) return static_cast<Object*>(value.as_function());
    if (value.is_object()) return value.as_object();
    Object* constructor = ctx.get_built_in_object("Object");
    if (!constructor || !constructor->is_function()) {
        ctx.throw_type_error("Cannot convert the value to an object");
        return nullptr;
    }
    Value boxed = static_cast<Function*>(constructor)->call(ctx, {value});
    if (ctx.has_exception()) return nullptr;
    return boxed.is_function() ? static_cast<Object*>(boxed.as_function()) : boxed.as_object_or_null();
}

Object* get_options_object(Context& ctx, const Value& options) {
    if (options.is_undefined()) {
        auto empty = ObjectFactory::create_object();
        empty->initialize_prototype(nullptr);
        return empty.release();
    }
    if (options.is_function()) return static_cast<Object*>(options.as_function());
    if (options.is_object()) return options.as_object();
    ctx.throw_type_error("Options must be an object or undefined");
    return nullptr;
}

Object* coerce_options_to_object(Context& ctx, const Value& options) {
    if (options.is_undefined()) {
        auto empty = ObjectFactory::create_object();
        empty->initialize_prototype(nullptr);
        return empty.release();
    }
    if (options.is_function()) return static_cast<Object*>(options.as_function());
    if (options.is_null()) { ctx.throw_type_error("Cannot convert null to an object"); return nullptr; }
    return to_object(ctx, options);
}

bool get_string_option(Context& ctx, Object* options, const char* property, std::initializer_list<const char*> allowed,
                       std::optional<std::string>& out) {
    out.reset();
    Value value = options->get_property(property);
    if (ctx.has_exception()) return false;
    if (value.is_undefined()) return true;
    std::string text;
    if (!value.to_string_checked(ctx, text)) return false;
    if (allowed.size() != 0) {
        bool ok = false;
        for (const char* candidate : allowed) {
            if (text == candidate) { ok = true; break; }
        }
        if (!ok) return range_error(ctx, std::string("Value ") + text + " out of range for option " + property);
    }
    out = std::move(text);
    return true;
}

bool get_boolean_option(Context& ctx, Object* options, const char* property, std::optional<bool>& out) {
    out.reset();
    Value value = options->get_property(property);
    if (ctx.has_exception()) return false;
    if (value.is_undefined()) return true;
    out = value.to_boolean();
    return true;
}

bool default_number_option(Context& ctx, const Value& value, double minimum, double maximum,
                           std::optional<double> fallback, std::optional<double>& out, const char* property) {
    out = fallback;
    if (value.is_undefined()) return true;
    double number;
    if (!value.to_number_checked(ctx, number)) return false;
    if (std::isnan(number) || number < minimum || number > maximum) {
        return range_error(ctx, std::string("Option ") + property + " is out of range");
    }
    out = std::floor(number);
    return true;
}

bool get_number_option(Context& ctx, Object* options, const char* property, double minimum, double maximum,
                       std::optional<double> fallback, std::optional<double>& out) {
    Value value = options->get_property(property);
    if (ctx.has_exception()) return false;
    return default_number_option(ctx, value, minimum, maximum, fallback, out, property);
}

// ---- Locales ---------------------------------------------------------------------------------------------------

const std::string* locale_object_tag(const Value& value);

bool canonicalize_tag_or_throw(Context& ctx, std::string_view tag, std::string& out) {
    std::optional<std::string> canonical = IntlICU::canonicalize_tag(tag);
    if (!canonical) return range_error(ctx, "Incorrect locale information provided");
    out = std::move(*canonical);
    return true;
}

bool canonicalize_locale_list(Context& ctx, const Value& locales, std::vector<std::string>& out) {
    out.clear();
    if (locales.is_undefined()) return true;
    auto add = [&](const std::string& tag) {
        if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
    };
    if (locales.is_string() || locale_object_tag(locales)) {
        std::string tag;
        if (const std::string* held = locale_object_tag(locales)) {
            tag = *held;
        } else if (!locales.to_string_checked(ctx, tag)) {
            return false;
        }
        std::string canonical;
        if (!canonicalize_tag_or_throw(ctx, tag, canonical)) return false;
        add(canonical);
        return true;
    }
    if (locales.is_null()) return type_error(ctx, "Cannot convert null to an object");
    Object* list = to_object(ctx, locales);
    if (!list) return false;
    Value length_value = list->get_property("length");
    if (ctx.has_exception()) return false;
    double length_number;
    if (!length_value.to_number_checked(ctx, length_number)) return false;
    double length = std::isnan(length_number) ? 0 : std::min(std::max(std::trunc(length_number), 0.0), 9007199254740991.0);
    for (double k = 0; k < length; k++) {
        const std::string key = std::to_string(static_cast<uint64_t>(k));
        bool present = list->has_property(key);
        if (ctx.has_exception()) return false;
        if (!present) continue;
        Value entry = list->get_property(key);
        if (ctx.has_exception()) return false;
        if (!entry.is_string() && !entry.is_object() && !entry.is_function()) {
            return type_error(ctx, "Locale list elements must be strings or objects");
        }
        std::string tag;
        if (const std::string* held = locale_object_tag(entry)) {
            tag = *held;
        } else if (!entry.to_string_checked(ctx, tag)) {
            return false;
        }
        std::string canonical;
        if (!canonicalize_tag_or_throw(ctx, tag, canonical)) return false;
        add(canonical);
    }
    return true;
}

const std::string* Resolved::value_of(std::string_view key) const {
    for (const auto& kv : values) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

bool get_matcher(Context& ctx, Object* options, Matcher& out) {
    std::optional<std::string> matcher;
    if (!get_string_option(ctx, options, "localeMatcher", {"lookup", "best fit"}, matcher)) return false;
    out = matcher && *matcher == "lookup" ? Matcher::Lookup : Matcher::BestFit;
    return true;
}

bool supported_locales(Context& ctx, const std::vector<std::string>& requested, const Value& options, Value& result) {
    Object* coerced = coerce_options_to_object(ctx, options);
    if (!coerced) return false;
    Matcher matcher;
    if (!get_matcher(ctx, coerced, matcher)) return false;
    std::vector<std::string> supported;
    for (const std::string& locale : requested) {
        if (!IntlICU::available_locale(locale).empty()) supported.push_back(locale);
    }
    result = string_array(supported);
    return true;
}

Value string_array(const std::vector<std::string>& items) {
    auto array = ObjectFactory::create_array(0);
    for (size_t i = 0; i < items.size(); i++) array->set_element(static_cast<uint32_t>(i), Value(items[i]));
    return Value(array.release());
}

void add_supported_locales_of(Context&, Function* constructor) {
    define_method(constructor, "supportedLocalesOf", 1, [](Context& ctx, std::span<const Value> args, Value) -> Value {
        std::vector<std::string> requested;
        if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
        Value result;
        if (!supported_locales(ctx, requested, args.size() > 1 ? args[1] : Value(), result)) return Value();
        return result;
    });
}

bool read_locales_and_options(Context& ctx, std::span<const Value> args, std::vector<std::string>& requested, Object*& options) {
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return false;
    options = coerce_options_to_object(ctx, args.size() > 1 ? args[1] : Value());
    return options != nullptr;
}

bool iterate(Context& ctx, const Value& iterable, const std::function<bool(const Value&)>& each) {
    Object* object = iterable.is_function() ? static_cast<Object*>(iterable.as_function()) : iterable.as_object_or_null();
    Value method;
    if (object) {
        Symbol* symbol = Symbol::get_well_known(Symbol::ITERATOR);
        method = symbol ? object->get_property(symbol->to_property_key()) : Value();
    } else if (iterable.is_string()) {
        Object* boxed = to_object(ctx, iterable);
        if (!boxed) return false;
        Symbol* symbol = Symbol::get_well_known(Symbol::ITERATOR);
        method = symbol ? boxed->get_property(symbol->to_property_key()) : Value();
    }
    if (ctx.has_exception()) return false;
    if (!method.is_function()) return type_error(ctx, "The value is not iterable");
    Value iterator = method.as_function()->call(ctx, {}, iterable);
    if (ctx.has_exception()) return false;
    Object* it = iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : iterator.as_object_or_null();
    if (!it) return type_error(ctx, "Result of the Symbol.iterator method is not an object");
    Value next = it->get_property("next");
    if (ctx.has_exception()) return false;
    while (true) {
        if (!next.is_function()) return type_error(ctx, "The iterator has no next method");
        Value step = next.as_function()->call(ctx, {}, iterator);
        if (ctx.has_exception()) return false;
        Object* result = step.is_function() ? static_cast<Object*>(step.as_function()) : step.as_object_or_null();
        if (!result) return type_error(ctx, "Iterator result is not an object");
        Value done = result->get_property("done");
        if (ctx.has_exception()) return false;
        if (done.to_boolean()) return true;
        Value value = result->get_property("value");
        if (ctx.has_exception()) return false;
        if (!each(value)) {
            // IteratorClose: the error already pending wins over anything "return" does.
            Value saved = ctx.get_exception();
            ctx.clear_exception();
            Value close = it->get_property("return");
            if (!ctx.has_exception() && close.is_function()) close.as_function()->call(ctx, {}, iterator);
            ctx.clear_exception();
            ctx.throw_exception(saved, true);
            return false;
        }
    }
}

void put(Object* object, const char* name, const Value& value) {
    object->set_property_descriptor(name, PropertyDescriptor(value, PropertyAttributes::Default));
}

Symbol* legacy_fallback_symbol(Context& ctx) {
    Realm* realm = ctx.realm();
    if (!realm) {
        static Symbol* orphan = Symbol::create("IntlLegacyConstructedSymbol").release();
        return orphan;
    }
    if (!realm->intl_fallback_symbol) realm->intl_fallback_symbol = Symbol::create("IntlLegacyConstructedSymbol").release();
    return realm->intl_fallback_symbol;
}

Value make_bound_function(Context&, DOMObject* owner, uint32_t length,
                          std::function<Value(Context&, std::span<const Value>, DOMObject*)> body) {
    auto function = ObjectFactory::create_native_function("",
        [owner, body = std::move(body)](Context& ctx, std::span<const Value> args, Value) -> Value {
            return body(ctx, args, owner);
        }, length);
    function->set_internal_slot("intl-owner", Value(static_cast<Object*>(owner)));
    return Value(function.release());
}

Object* make_object(std::initializer_list<std::pair<const char*, Value>> properties) {
    auto object = ObjectFactory::create_object();
    for (const auto& [name, value] : properties) put(object.get(), name, value);
    return object.release();
}

// ---- Building objects ------------------------------------------------------------------------------------------

void define_method(Object* target, const char* name, uint32_t length, NativeFn fn) {
    auto function = ObjectFactory::create_native_function(name, std::move(fn), length);
    target->set_property(name, Value(function.release()), PropertyAttributes::BuiltinFunction);
}

void define_getter(Object* target, const char* name, NativeFn fn) {
    auto getter = ObjectFactory::create_native_function(std::string("get ") + name, std::move(fn), 0);
    PropertyDescriptor descriptor;
    descriptor.set_getter(getter.release());
    descriptor.set_enumerable(false);
    descriptor.set_configurable(true);
    target->set_property_descriptor(name, descriptor);
}

void define_to_string_tag(Object* target, const char* tag) {
    if (Symbol* symbol = Symbol::get_well_known(Symbol::TO_STRING_TAG)) {
        PropertyDescriptor descriptor(Value(std::string(tag)),
                                      static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        target->set_property_descriptor(symbol->to_property_key(), descriptor);
    }
}

Class define_class(Context& ctx, Object* intl, const char* name, uint32_t length, Kind kind, ConstructFn construct) {
    auto constructor = ObjectFactory::create_native_constructor_with_new_target(name, std::move(construct), length);
    auto prototype = ObjectFactory::create_object();
    Function* constructor_ptr = constructor.get();
    Object* prototype_ptr = prototype.get();
    prototype->set_property("constructor", Value(constructor_ptr), PropertyAttributes::BuiltinFunction);
    constructor->set_property("prototype", Value(prototype_ptr), PropertyAttributes::None);
    define_to_string_tag(prototype_ptr, (std::string("Intl.") + name).c_str());
    if (Realm* realm = ctx.realm()) realm->intl_prototypes[static_cast<size_t>(kind)] = prototype_ptr;
    intl->set_property(name, Value(constructor.release()), PropertyAttributes::BuiltinFunction);
    prototype.release();
    return Class{constructor_ptr, prototype_ptr};
}

// ---- Locale negotiation ----------------------------------------------------------------------------------------

namespace {

// LookupMatcher: the first requested locale with data. The unicode extension of that request rides along.
struct Match {
    std::string locale;       // the available locale, without extensions
    std::string data_locale;  // the locale its data is under
    std::optional<LocaleId> extension_source;
};

Match lookup_match(const std::vector<std::string>& requested) {
    for (const std::string& tag : requested) {
        std::string available = IntlICU::available_locale(tag);
        if (available.empty()) continue;
        Match match;
        match.locale = available;
        match.data_locale = IntlICU::best_available(tag);
        LocaleId id = IntlICU::split_tag(tag);
        if (!id.attributes.empty() || !id.keywords.empty()) match.extension_source = std::move(id);
        return match;
    }
    Match match;
    const std::string& fallback = IntlICU::default_locale();
    match.locale = IntlICU::available_locale(fallback);
    match.data_locale = IntlICU::best_available(fallback);
    if (match.locale.empty()) match.locale = match.data_locale = "en";
    return match;
}

}  // namespace

Resolved resolve_locale(const std::vector<std::string>& requested, const std::vector<ExtensionKey>& keys,
                        const std::vector<std::pair<std::string, std::string>>& overrides, Matcher) {
    Match match = lookup_match(requested);
    Resolved result;
    result.data_locale = match.data_locale;
    std::vector<std::pair<std::string, std::string>> kept;   // the keywords the found locale will carry
    for (const ExtensionKey& key : keys) {
        const std::vector<std::string> supported = key.supported(match.data_locale);
        std::string value = supported.empty() ? std::string() : supported[0];
        std::optional<std::pair<std::string, std::string>> addition;
        if (match.extension_source) {
            if (const std::string* requested_value = match.extension_source->keyword(key.key)) {
                if (!requested_value->empty()) {
                    if (std::find(supported.begin(), supported.end(), *requested_value) != supported.end()) {
                        value = *requested_value;
                        addition = std::make_pair(std::string(key.key), *requested_value);
                    }
                } else if (std::find(supported.begin(), supported.end(), "true") != supported.end()) {
                    value = "true";
                    addition = std::make_pair(std::string(key.key), std::string());
                }
            }
        }
        for (const auto& [override_key, override_value] : overrides) {
            if (override_key != key.key) continue;
            if (std::find(supported.begin(), supported.end(), override_value) != supported.end() && override_value != value) {
                value = override_value;
                addition.reset();
            }
        }
        result.values.emplace_back(key.key, value);
        if (addition) kept.push_back(*addition);
    }
    std::string locale = match.locale;
    if (!kept.empty()) {
        LocaleId id = IntlICU::split_tag(locale);
        id.keywords = kept;
        std::sort(id.keywords.begin(), id.keywords.end());
        locale = id.to_string();
    }
    result.locale = locale;
    return result;
}

void register_all(Context& ctx, Object* intl) {
    define_to_string_tag(intl, "Intl");
    define_method(intl, "getCanonicalLocales", 1, [](Context& ctx, std::span<const Value> args, Value) -> Value {
        std::vector<std::string> locales;
        if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], locales)) return Value();
        return string_array(locales);
    });
    define_method(intl, "supportedValuesOf", 1, [](Context& ctx, std::span<const Value> args, Value) -> Value {
        std::string key;
        if (!(args.empty() ? Value() : args[0]).to_string_checked(ctx, key)) return Value();
        std::vector<std::string> values = supported_values(key);
        if (values.empty()) { ctx.throw_range_error("Invalid key : " + key); return Value(); }
        return string_array(values);
    });
    register_locale(ctx, intl);
    register_plural_rules(ctx, intl);
    register_list_format(ctx, intl);
    register_collator(ctx, intl);
    register_segmenter(ctx, intl);
    register_number_format(ctx, intl);
    register_relative_time_format(ctx, intl);
    register_display_names(ctx, intl);
    register_date_time_format(ctx, intl);
    register_duration_format(ctx, intl);
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
