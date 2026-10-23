/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlNumber.h"

#include "icu4x/ListFormatter.hpp"
#include "icu4x/ListLength.hpp"
#include "icu4x/Locale.hpp"

namespace Quanta::IntlImpl {

namespace {

struct ListFormatObject : DOMObject {
    std::string locale, type, style;
    std::unique_ptr<icu4x::ListFormatter> formatter;
};

Value construct_list_format(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.ListFormat requires 'new'"); return Value(); }
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
    Object* options = get_options_object(ctx, args.size() > 1 ? args[1] : Value());
    if (!options) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> type, style;
    if (!get_string_option(ctx, options, "type", {"conjunction", "disjunction", "unit"}, type)) return Value();
    if (!get_string_option(ctx, options, "style", {"long", "short", "narrow"}, style)) return Value();
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::ListFormat);
    if (ctx.has_exception()) return Value();

    Resolved resolved = resolve_locale(requested, {}, {}, matcher);
    auto* object = Heap::Allocate<ListFormatObject>();
    object->locale = resolved.locale;
    object->type = type ? *type : "conjunction";
    object->style = style ? *style : "long";
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(resolved.data_locale);
    if (!locale) { ctx.throw_range_error("Incorrect locale information provided"); return Value(); }
    const icu4x::ListLength length = object->style == "long" ? icu4x::ListLength::Wide
        : object->style == "short" ? icu4x::ListLength::Short : icu4x::ListLength::Narrow;
    const auto& provider = IntlICU::provider();
    auto created = object->type == "conjunction" ? icu4x::ListFormatter::create_and_with_length_and_provider(provider, *locale, length)
        : object->type == "disjunction" ? icu4x::ListFormatter::create_or_with_length_and_provider(provider, *locale, length)
        : icu4x::ListFormatter::create_unit_with_length_and_provider(provider, *locale, length);
    if (!created.is_ok()) { ctx.throw_range_error("List formatting is not available for this locale"); return Value(); }
    object->formatter = std::move(created).ok().value();
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

// StringListFromIterable. An element that is not a string is a TypeError and closes the iterator.
bool string_list(Context& ctx, const Value& iterable, std::vector<std::string>& out) {
    out.clear();
    if (iterable.is_undefined()) return true;
    return iterate(ctx, iterable, [&](const Value& element) {
        if (!element.is_string()) return type_error(ctx, "Iterable yielded a value that is not a string");
        out.push_back(element.to_string());
        return true;
    });
}

// The list formatted, with the parts told apart. Each element is followed by a marker no locale data uses, which says
// where it ends; formatting the elements themselves (not placeholders) keeps rules that look at the next word.
struct ListPart { bool element; std::string text; };

std::vector<ListPart> format_parts(const ListFormatObject& self, const std::vector<std::string>& items) {
    static const std::string marker = "\xEE\x80\x80";   // U+E000
    std::vector<std::string> marked;
    for (const std::string& item : items) marked.push_back(item + marker);
    std::vector<icu4x::diplomat::string_view_for_slice> views;
    for (const std::string& m : marked) views.emplace_back(m);
    std::string output = self.formatter->format(views);

    std::vector<ListPart> parts;
    size_t cursor = 0;
    for (const std::string& item : items) {
        size_t at = output.find(marker, cursor);
        if (at == std::string::npos || at < cursor + item.size()) break;
        const size_t start = at - item.size();
        if (start > cursor) parts.push_back({false, output.substr(cursor, start - cursor)});
        parts.push_back({true, item});
        cursor = at + marker.size();
    }
    if (cursor < output.size()) parts.push_back({false, output.substr(cursor)});
    return parts;
}

}  // namespace

void register_list_format(Context& ctx, Object* intl) {
    Class list = define_class(ctx, intl, "ListFormat", 0, Kind::ListFormat, construct_list_format);
    add_supported_locales_of(ctx, list.constructor);
    Object* proto = list.prototype;

    define_method(proto, "format", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<ListFormatObject>(ctx, receiver, "ListFormat");
        if (!self) return Value();
        std::vector<std::string> items;
        if (!string_list(ctx, args.empty() ? Value() : args[0], items)) return Value();
        std::string text;
        for (const ListPart& part : format_parts(*self, items)) text += part.text;
        return Value(text);
    });

    define_method(proto, "formatToParts", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<ListFormatObject>(ctx, receiver, "ListFormat");
        if (!self) return Value();
        std::vector<std::string> items;
        if (!string_list(ctx, args.empty() ? Value() : args[0], items)) return Value();
        auto array = ObjectFactory::create_array(0);
        uint32_t index = 0;
        for (const ListPart& part : format_parts(*self, items)) {
            array->set_element(index++, Value(make_object({
                {"type", Value(part.element ? "element" : "literal")},
                {"value", Value(part.text)},
            })));
        }
        return Value(array.release());
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<ListFormatObject>(ctx, receiver, "ListFormat");
        if (!self) return Value();
        return Value(make_object({
            {"locale", Value(self->locale)}, {"type", Value(self->type)}, {"style", Value(self->style)},
        }));
    });
}

bool list_format_parts(Context& ctx, const std::string& locale, const char* type, const std::string& style,
                       const std::vector<std::string>& items, std::vector<std::pair<bool, std::string>>& out) {
    Object* bag = make_object({{"type", Value(type)}, {"style", Value(style)}});
    const Value arguments[2] = {Value(locale), Value(bag)};
    Value created = construct_list_format(ctx, arguments, Value(), true, Value());
    if (ctx.has_exception()) return false;
    ListFormatObject* list = DOMObject::Cast<ListFormatObject>(created);
    if (!list) return false;
    for (const ListPart& part : format_parts(*list, items)) out.emplace_back(part.element, part.text);
    return true;
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
