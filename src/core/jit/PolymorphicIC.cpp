#include "quanta/core/jit/PolymorphicIC.h"
#include <iostream>

namespace Quanta {

Value PolymorphicIC::load_property(Object* obj, const std::string& prop) {
    if (!obj) return Value();

    void* shape = static_cast<void*>(obj);
    access_count_++;

    if (state_ == ICState::UNINITIALIZED) {
        Value val = obj->get_property(prop);
        ICEntry entry = {shape, 0, val};
        entries_.push_back(entry);
        state_ = ICState::MONOMORPHIC;
        std::cout << "[IC] Initialized monomorphic cache for " << prop << std::endl;
        return val;
    }

    if (state_ == ICState::MONOMORPHIC) {
        if (entries_[0].object_shape == shape) {
            return obj->get_property(prop);
        }

        transition_to_polymorphic();
        Value val = obj->get_property(prop);
        ICEntry entry = {shape, 0, val};
        entries_.push_back(entry);
        std::cout << "[IC] Transitioned to polymorphic for " << prop << std::endl;
        return val;
    }

    if (state_ == ICState::POLYMORPHIC) {
        ICEntry* found = find_entry(shape);
        if (found) {
            return obj->get_property(prop);
        }

        if (entries_.size() >= MAX_POLYMORPHIC) {
            transition_to_megamorphic();
            std::cout << "[IC] Transitioned to megamorphic for " << prop << std::endl;
        } else {
            Value val = obj->get_property(prop);
            ICEntry entry = {shape, 0, val};
            entries_.push_back(entry);
        }
    }

    return obj->get_property(prop);
}

void PolymorphicIC::store_property(Object* obj, const std::string& prop, const Value& val) {
    if (!obj) return;

    void* shape = static_cast<void*>(obj);

    if (state_ == ICState::MEGAMORPHIC) {
        obj->set_property(prop, val);
        return;
    }

    ICEntry* found = find_entry(shape);
    if (found) {
        found->cached_value = val;
    }

    obj->set_property(prop, val);
}

void PolymorphicIC::transition_to_polymorphic() {
    state_ = ICState::POLYMORPHIC;
}

void PolymorphicIC::transition_to_megamorphic() {
    state_ = ICState::MEGAMORPHIC;
    entries_.clear();
}

ICEntry* PolymorphicIC::find_entry(void* shape) {
    for (auto& entry : entries_) {
        if (entry.object_shape == shape) {
            return &entry;
        }
    }
    return nullptr;
}

}
