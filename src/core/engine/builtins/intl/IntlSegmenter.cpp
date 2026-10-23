/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef QUANTA_ICU4X

#include "IntlSupport.h"

#include "quanta/core/engine/Realm.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Symbol.h"

#include "icu4x/GraphemeClusterBreakIteratorUtf16.hpp"
#include "icu4x/GraphemeClusterSegmenter.hpp"
#include "icu4x/Locale.hpp"
#include "icu4x/SentenceBreakIteratorUtf16.hpp"
#include "icu4x/SentenceSegmenter.hpp"
#include "icu4x/WordBreakIteratorUtf16.hpp"
#include "icu4x/WordSegmenter.hpp"

#include <cmath>

namespace Quanta::IntlImpl {

namespace {

enum class Granularity : uint8_t { Grapheme, Word, Sentence };

struct SegmenterObject : DOMObject {
    std::string locale;
    Granularity granularity = Granularity::Grapheme;
    std::unique_ptr<icu4x::GraphemeClusterSegmenter> grapheme;
    std::unique_ptr<icu4x::WordSegmenter> word;
    std::unique_ptr<icu4x::SentenceSegmenter> sentence;
};

// A string cut up: boundaries[0] is 0 and the last is the length; word_like[i] describes the segment that ends at
// boundaries[i + 1].
struct SegmentsObject : DOMObject {
    std::string text;
    std::u16string units;
    Granularity granularity = Granularity::Grapheme;
    std::vector<int32_t> boundaries;
    std::vector<bool> word_like;
};

struct SegmentIteratorObject : DOMObject {
    TracedValue segments;
    size_t next = 0;   // index of the segment to hand out
};

Value segment_data(const SegmentsObject& segments, size_t i) {
    const int32_t start = segments.boundaries[i], end = segments.boundaries[i + 1];
    Object* data = make_object({
        {"segment", Value(utf16_to_wtf8(segments.units.data() + start, static_cast<size_t>(end - start)))},
        {"index", Value(static_cast<double>(start))},
        {"input", Value(segments.text)},
    });
    if (segments.granularity == Granularity::Word) put(data, "isWordLike", Value(static_cast<bool>(segments.word_like[i])));
    return Value(data);
}

Value construct_segmenter(Context& ctx, std::span<const Value> args, Value, bool is_construct, Value new_target) {
    if (!is_construct) { ctx.throw_type_error("Constructor Intl.Segmenter requires 'new'"); return Value(); }
    std::vector<std::string> requested;
    if (!canonicalize_locale_list(ctx, args.empty() ? Value() : args[0], requested)) return Value();
    Object* options = get_options_object(ctx, args.size() > 1 ? args[1] : Value());
    if (!options) return Value();
    Matcher matcher;
    if (!get_matcher(ctx, options, matcher)) return Value();
    std::optional<std::string> granularity;
    if (!get_string_option(ctx, options, "granularity", {"grapheme", "word", "sentence"}, granularity)) return Value();
    Object* proto = prototype_from_new_target(ctx, new_target, Kind::Segmenter);
    if (ctx.has_exception()) return Value();

    Resolved resolved = resolve_locale(requested, {}, {}, matcher);
    auto* object = Heap::Allocate<SegmenterObject>();
    object->locale = resolved.locale;
    object->granularity = !granularity || *granularity == "grapheme" ? Granularity::Grapheme
        : *granularity == "word" ? Granularity::Word : Granularity::Sentence;
    std::unique_ptr<icu4x::Locale> locale = IntlICU::to_icu(resolved.data_locale);
    if (!locale) { ctx.throw_range_error("Incorrect locale information provided"); return Value(); }
    const auto& provider = IntlICU::provider();
    bool ok = true;
    switch (object->granularity) {
        case Granularity::Grapheme: {
            auto made = icu4x::GraphemeClusterSegmenter::create_with_provider(provider);
            if ((ok = made.is_ok())) object->grapheme = std::move(made).ok().value();
            break;
        }
        case Granularity::Word: {
            auto made = icu4x::WordSegmenter::create_auto_with_content_locale_and_provider(provider, *locale);
            if ((ok = made.is_ok())) object->word = std::move(made).ok().value();
            break;
        }
        case Granularity::Sentence: {
            auto made = icu4x::SentenceSegmenter::create_with_content_locale_and_provider(provider, *locale);
            if ((ok = made.is_ok())) object->sentence = std::move(made).ok().value();
            break;
        }
    }
    if (!ok) { ctx.throw_range_error("Segmentation is not available for this locale"); return Value(); }
    if (proto) object->initialize_prototype(proto);
    return Value(static_cast<Object*>(object));
}

SegmentIteratorObject* iterator_of(Context& ctx, const Value& receiver) {
    return this_as<SegmentIteratorObject>(ctx, receiver, "Segmenter String Iterator");
}

}  // namespace

void register_segmenter(Context& ctx, Object* intl) {
    Class segmenter = define_class(ctx, intl, "Segmenter", 0, Kind::Segmenter, construct_segmenter);
    add_supported_locales_of(ctx, segmenter.constructor);
    Object* proto = segmenter.prototype;

    // %SegmentsPrototype% and %SegmentIteratorPrototype% are not reachable from the global object.
    Realm* realm = ctx.realm();
    auto segments_proto = ObjectFactory::create_object();
    auto iterator_proto = ObjectFactory::create_object();
    iterator_proto->initialize_prototype(Iterator::s_iterator_prototype_());
    if (realm) {
        realm->intl_prototypes[static_cast<size_t>(Kind::Segments)] = segments_proto.get();
        realm->intl_prototypes[static_cast<size_t>(Kind::SegmentIterator)] = iterator_proto.get();
    }
    define_to_string_tag(iterator_proto.get(), "Segmenter String Iterator");

    define_method(proto, "segment", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<SegmenterObject>(ctx, receiver, "Segmenter");
        if (!self) return Value();
        std::string text;
        if (!(args.empty() ? Value() : args[0]).to_string_checked(ctx, text)) return Value();
        auto* segments = Heap::Allocate<SegmentsObject>();
        segments->text = text;
        segments->units = wtf8_to_utf16(text);
        segments->granularity = self->granularity;
        const std::u16string_view view(segments->units);
        std::vector<int32_t>& bounds = segments->boundaries;
        bounds.push_back(0);
        auto gather = [&](auto& iterator, auto&& word_like) {
            while (true) {
                const int32_t at = iterator.next();
                if (at < 0) break;
                if (at == bounds.back()) continue;
                bounds.push_back(at);
                segments->word_like.push_back(word_like(iterator));
            }
        };
        switch (self->granularity) {
            case Granularity::Grapheme: {
                auto iterator = self->grapheme->segment16(view);
                gather(*iterator, [](auto&) { return false; });
                break;
            }
            case Granularity::Word: {
                auto iterator = self->word->segment16(view);
                gather(*iterator, [](auto& it) { return it.is_word_like(); });
                break;
            }
            case Granularity::Sentence: {
                auto iterator = self->sentence->segment16(view);
                gather(*iterator, [](auto&) { return false; });
                break;
            }
        }
        if (bounds.back() != static_cast<int32_t>(view.size())) {
            bounds.push_back(static_cast<int32_t>(view.size()));
            segments->word_like.push_back(false);
        }
        if (Object* p = prototype_of(ctx, Kind::Segments)) segments->initialize_prototype(p);
        return Value(static_cast<Object*>(segments));
    });

    define_method(proto, "resolvedOptions", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        auto* self = this_as<SegmenterObject>(ctx, receiver, "Segmenter");
        if (!self) return Value();
        const char* names[] = {"grapheme", "word", "sentence"};
        return Value(make_object({
            {"locale", Value(self->locale)},
            {"granularity", Value(names[static_cast<int>(self->granularity)])},
        }));
    });

    define_method(segments_proto.get(), "containing", 1, [](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
        auto* self = this_as<SegmentsObject>(ctx, receiver, "Segments");
        if (!self) return Value();
        double n = 0;
        if (!(args.empty() ? Value() : args[0]).to_number_checked(ctx, n)) return Value();
        n = std::isnan(n) ? 0 : std::trunc(n);
        if (n < 0 || n >= static_cast<double>(self->units.size())) return Value();
        const auto& b = self->boundaries;
        const size_t i = static_cast<size_t>(std::upper_bound(b.begin(), b.end(), static_cast<int32_t>(n)) - b.begin()) - 1;
        return segment_data(*self, i);
    });

    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    auto make_iterator = ObjectFactory::create_native_function("[Symbol.iterator]",
        [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
            auto* self = this_as<SegmentsObject>(ctx, receiver, "Segments");
            if (!self) return Value();
            auto* iterator = Heap::Allocate<SegmentIteratorObject>();
            iterator->segments.Set(receiver);
            if (Object* p = prototype_of(ctx, Kind::SegmentIterator)) iterator->initialize_prototype(p);
            return Value(static_cast<Object*>(iterator));
        }, 0);
    if (iterator_symbol) {
        segments_proto->set_property(iterator_symbol->to_property_key(), Value(make_iterator.release()),
                                     PropertyAttributes::BuiltinFunction);
    }

    define_method(iterator_proto.get(), "next", 0, [](Context& ctx, std::span<const Value>, Value receiver) -> Value {
        SegmentIteratorObject* self = iterator_of(ctx, receiver);
        if (!self) return Value();
        auto* segments = DOMObject::Cast<SegmentsObject>(self->segments.Get());
        const bool done = !segments || self->next + 1 >= segments->boundaries.size();
        return Value(make_object({
            {"value", done ? Value() : segment_data(*segments, self->next++)},
            {"done", Value(done)},
        }));
    });
    // The prototypes are held by the realm's Intl table, which the collector traces.
    segments_proto.release();
    iterator_proto.release();
}

}  // namespace Quanta::IntlImpl

#endif  // QUANTA_ICU4X
