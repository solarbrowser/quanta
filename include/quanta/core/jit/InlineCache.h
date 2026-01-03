/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <string>
#include <unordered_map>

namespace Quanta {

struct FastPropertyCache {
    void* last_object;
    std::string last_property;
    Value cached_value;
    bool valid;
};

struct TypeFeedbackSite {
    enum class Type {
        NONE, INT, DOUBLE, STRING, OBJECT, ARRAY
    };

    Type observed_type;
    int observation_count;
    bool is_monomorphic;
    bool is_polymorphic;
};

class InlineCache {
private:
    static constexpr size_t CACHE_SIZE = 256;
    FastPropertyCache property_cache_[CACHE_SIZE];
    std::unordered_map<void*, TypeFeedbackSite> type_feedback_;

public:
    InlineCache();

    Value get_property_cached(Object* obj, const char* prop, uint32_t slot);
    void record_type_feedback(void* site, TypeFeedbackSite::Type type);
    TypeFeedbackSite get_feedback(void* site);
    void clear();
};

extern "C" {
    Value jit_get_property_cached(Object* obj, const char* prop, uint32_t cache_slot);
    void record_type_feedback(void* site, int type_tag);
}

}
