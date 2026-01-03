/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/jit/InlineCache.h"

namespace Quanta {

static FastPropertyCache fast_prop_cache[256];
static std::unordered_map<void*, TypeFeedbackSite> type_feedback;

InlineCache::InlineCache() {
    clear();
}

Value InlineCache::get_property_cached(Object* obj, const char* prop, uint32_t slot) {
    if (!obj || !prop) return Value();

    FastPropertyCache& cache = property_cache_[slot % CACHE_SIZE];

    if (cache.valid && cache.last_object == obj && cache.last_property == prop) {
        return cache.cached_value;
    }

    Value result = obj->get_property(prop);
    cache.last_object = obj;
    cache.last_property = prop;
    cache.cached_value = result;
    cache.valid = true;

    return result;
}

void InlineCache::record_type_feedback(void* site, TypeFeedbackSite::Type type) {
    auto& feedback = type_feedback_[site];

    if (feedback.observation_count == 0) {
        feedback.observed_type = type;
        feedback.is_monomorphic = true;
    } else if (feedback.observed_type != type) {
        feedback.is_monomorphic = false;
        feedback.is_polymorphic = true;
    }

    feedback.observation_count++;
}

TypeFeedbackSite InlineCache::get_feedback(void* site) {
    return type_feedback_[site];
}

void InlineCache::clear() {
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        property_cache_[i].valid = false;
    }
    type_feedback_.clear();
}

extern "C" Value jit_get_property_cached(Object* obj, const char* prop, uint32_t cache_slot) {
    if (!obj || !prop) return Value();

    FastPropertyCache& cache = fast_prop_cache[cache_slot % 256];

    if (cache.valid && cache.last_object == obj && cache.last_property == prop) {
        return cache.cached_value;
    }

    Value result = obj->get_property(prop);
    cache.last_object = obj;
    cache.last_property = prop;
    cache.cached_value = result;
    cache.valid = true;

    return result;
}

extern "C" void record_type_feedback(void* site, int type_tag) {
    auto& feedback = type_feedback[site];
    TypeFeedbackSite::Type current = static_cast<TypeFeedbackSite::Type>(type_tag);

    if (feedback.observation_count == 0) {
        feedback.observed_type = current;
        feedback.is_monomorphic = true;
    } else if (feedback.observed_type != current) {
        feedback.is_monomorphic = false;
        feedback.is_polymorphic = true;
    }

    feedback.observation_count++;
}

}
