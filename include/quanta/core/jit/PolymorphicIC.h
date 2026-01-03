#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <vector>
#include <string>

namespace Quanta {

struct ICEntry {
    void* object_shape;
    uint32_t offset;
    Value cached_value;
};

enum class ICState {
    UNINITIALIZED,
    MONOMORPHIC,
    POLYMORPHIC,
    MEGAMORPHIC
};

class PolymorphicIC {
private:
    static constexpr int MAX_POLYMORPHIC = 4;
    std::vector<ICEntry> entries_;
    ICState state_;
    int access_count_;

public:
    PolymorphicIC() : state_(ICState::UNINITIALIZED), access_count_(0) {}

    Value load_property(Object* obj, const std::string& prop);
    void store_property(Object* obj, const std::string& prop, const Value& val);

    ICState get_state() const { return state_; }
    bool is_megamorphic() const { return state_ == ICState::MEGAMORPHIC; }

private:
    void transition_to_polymorphic();
    void transition_to_megamorphic();
    ICEntry* find_entry(void* shape);
};

}
