/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Realm.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/String.h"

namespace Quanta {

constinit thread_local uint32_t Realm::death_epoch_ = 0;

void Realm::trace(Visitor& v) const {
    v.visit_object(object_proto);
    v.visit_object(array_proto);
    v.visit_object(function_proto);
    v.visit_object(global_proxy);
    v.visit_object(pristine_call);
    v.visit_object(pristine_apply);
    v.visit_object(throw_type_error);
    for (Object* proto : primitive_protos) v.visit_object(proto);
    v.visit_object(intrinsic_promise);
    v.visit_object(iterator_proto);
    v.visit_object(array_iterator_proto);
    v.visit_object(string_iterator_proto);
    v.visit_object(map_iterator_proto);
    v.visit_object(set_iterator_proto);
    v.visit_object(generator_proto);
    v.visit_object(generator_function_proto);
    v.visit_object(generator_next_fn);
    v.visit_object(async_generator_proto);
    v.visit_object(async_generator_function_proto);
    v.visit_object(map_proto);
    v.visit_object(set_proto);
    v.visit_object(weakmap_proto);
    v.visit_object(weakset_proto);
    v.visit_object(weakref_proto);
    v.visit_object(array_buffer_proto);
    v.visit_object(finalization_registry_proto);
    v.visit_object(watched_array_iterator_proto);
    v.visit_object(watched_regexp_proto);
    v.visit_object(watched_promise_proto);
    v.visit_object(watched_promise_ctor);
    v.visit_string(legacy_regexp_.subject_cell);
}

void Realm::legacy_regexp_match(const std::string& subject, const String* cell,
                                const size_t* pairs, size_t n_pairs) {
    LegacyRegExp& l = legacy_regexp_;
    auto narrow = [](size_t v) { return v == SIZE_MAX ? kLegacyUnset : static_cast<uint32_t>(v); };
    l.state = LegacyRegExp::State::Matched;
    l.input_set = false;
    if (cell) {
        l.subject_cell = const_cast<String*>(cell);
        l.subject_text.clear();
    } else {
        l.subject_cell = nullptr;
        l.subject_text.assign(subject);
    }
    const size_t kept = n_pairs < 10 ? n_pairs : 10;
    for (size_t i = 0; i < kept; i++) {
        l.caps[2 * i] = narrow(pairs[2 * i]);
        l.caps[2 * i + 1] = narrow(pairs[2 * i + 1]);
    }
    for (size_t i = kept; i < 10; i++) l.caps[2 * i] = l.caps[2 * i + 1] = kLegacyUnset;
    l.group_count = n_pairs > 0 ? static_cast<uint32_t>(n_pairs - 1) : 0;
    if (n_pairs > 1) {
        l.last_pair[0] = narrow(pairs[2 * (n_pairs - 1)]);
        l.last_pair[1] = narrow(pairs[2 * (n_pairs - 1) + 1]);
    }
}

void Realm::legacy_regexp_invalidate() {
    LegacyRegExp& l = legacy_regexp_;
    l.state = LegacyRegExp::State::Invalid;
    l.input_set = false;
    l.subject_cell = nullptr;
    l.subject_text.clear();
}

void Realm::legacy_regexp_set_input(std::string value) {
    legacy_regexp_.input_set = true;
    legacy_regexp_.input_value = std::move(value);
}

bool Realm::legacy_regexp_get(LegacyRegExpSlot slot, std::string& out) const {
    const LegacyRegExp& l = legacy_regexp_;
    out.clear();
    if (slot == LegacyRegExpSlot::Input && l.input_set) { out = l.input_value; return true; }
    if (l.state == LegacyRegExp::State::Invalid) return false;
    if (l.state == LegacyRegExp::State::Initial) return true;
    const std::string& subject = l.subject_cell ? l.subject_cell->str() : l.subject_text;
    auto cut = [&](uint32_t from, uint32_t to) {
        if (from == kLegacyUnset || to == kLegacyUnset || to < from) return std::string();
        return utf16_substring(subject, from, to);
    };
    switch (slot) {
        case LegacyRegExpSlot::Input: out = subject; break;
        case LegacyRegExpSlot::LastMatch: out = cut(l.caps[0], l.caps[1]); break;
        case LegacyRegExpSlot::LastParen:
            if (l.group_count > 0) out = cut(l.last_pair[0], l.last_pair[1]);
            break;
        case LegacyRegExpSlot::LeftContext: out = cut(0, l.caps[0]); break;
        case LegacyRegExpSlot::RightContext: out = utf16_substring(subject, l.caps[1], utf16_length(subject)); break;
        default: {
            const size_t n = static_cast<size_t>(slot) - static_cast<size_t>(LegacyRegExpSlot::Paren1) + 1;
            if (n <= l.group_count && n < 10) out = cut(l.caps[2 * n], l.caps[2 * n + 1]);
        }
    }
    return true;
}

Realm& null_realm() {
    static thread_local Realm realm;
    return realm;
}

}
