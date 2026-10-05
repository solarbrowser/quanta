/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/DOMObject.h"
#include "quanta/core/gc/Collector.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Realm.h"

namespace Quanta {

namespace {

// The host object whose constructor is running, which a Traced member finds to attach itself to.
constinit thread_local DOMObject* g_constructing = nullptr;
// How many live host objects declared Finalize(); the pass over a collection's dead is skipped when none did.
constinit thread_local size_t g_finalizable_live = 0;

std::vector<std::weak_ptr<WeakHandleBase::Slot>>& weak_slots() {
    static thread_local std::vector<std::weak_ptr<WeakHandleBase::Slot>> slots;
    return slots;
}

}

DOMObject::DOMObject() : CustomObjectBase(ObjectType::Custom) {
    set_custom_kind(CustomKind::Host);
    g_constructing = this;
}

DOMObject* DOMObject::constructing() { return g_constructing; }
void DOMObject::set_constructing(DOMObject* object) { g_constructing = object; }
void DOMObject::note_finalizable() { g_finalizable_live++; }

void DOMObject::trace(Visitor& v) {
    Object::trace_default(v);
    for (const TracedBase* member = traced_; member; member = member->next_) member->Trace(v);
    if (type_) type_->visit(this, v);
}

// A cell that was never given a type (its constructor threw before
// Heap::Allocate could stamp it) has only the base to destroy.
void DOMObject::destroy() {
    if (type_ && type_->finalize) g_finalizable_live--;
    if (type_) type_->destroy(this);
    else this->~DOMObject();
}

bool DOMObject::finalization_pending() {
    return g_finalizable_live > 0 || !weak_slots().empty();
}

void DOMObject::finalize_dead(const std::vector<Heap::DeadCell>& dead) {
    auto& slots = weak_slots();
    if (!slots.empty()) {
        std::unordered_set<const void*> gone;
        gone.reserve(dead.size());
        for (const Heap::DeadCell& d : dead) {
            if (d.kind == CellKind::Object) gone.insert(d.cell);
        }
        size_t kept = 0;
        for (size_t i = 0; i < slots.size(); i++) {
            std::shared_ptr<WeakHandleBase::Slot> slot = slots[i].lock();
            if (!slot) continue;
            if (slot->target && gone.count(slot->target)) slot->target = nullptr;
            slots[kept++] = slots[i];
        }
        slots.resize(kept);
    }
    if (g_finalizable_live == 0) return;
    for (const Heap::DeadCell& d : dead) {
        if (d.kind != CellKind::Object) continue;
        Object* obj = static_cast<Object*>(d.cell);
        if (obj->get_type() != Object::ObjectType::Custom) continue;
        auto* base = static_cast<CustomObjectBase*>(obj);
        if (base->get_custom_kind() != CustomObjectBase::CustomKind::Host) continue;
        DOMObject* dom = static_cast<DOMObject*>(base);
        if (dom->type_ && dom->type_->finalize) dom->type_->finalize(dom);
    }
}

TracedBase::TracedBase() : owner_(g_constructing) {
    if (!owner_) {
        std::fprintf(stderr, "quanta: a Traced member was made outside the constructor of a DOMObject\n");
        std::abort();
    }
    next_ = owner_->traced_;
    owner_->traced_ = this;
}

void TracedBase::NoteStored(const Value& value) {
    owner_->NoteWrite(value);
}

WeakHandleBase::WeakHandleBase(Object* target) : slot_(std::make_shared<Slot>(Slot{target})) {
    weak_slots().push_back(slot_);
}

void DOMObject::NoteWrite() {
    Collector::write_barrier(this);
}

void DOMObject::NoteWrite(const Value& stored) {
    Collector::write_barrier_value(this, stored);
}

// ---- Legacy platform objects ----------------------------------------------------

namespace {

bool is_symbol_key(const std::string& key) {
    return key.rfind("@@sym:", 0) == 0 || key.rfind("Symbol.", 0) == 0;
}

// A canonical array index: "0", "7", "4294967294"; not "01", "-1", "1.5", "4294967295".
bool array_index(const std::string& key, uint32_t& out) {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    uint64_t value = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value >= 4294967295ull) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

// The hooks run on the script's context; with none running (a host reading a property from
// outside script) the realm's own stands in.
Context* hook_context() {
    if (Object::current_context_) return Object::current_context_;
    return g_current_realm ? g_current_realm->global_ctx : nullptr;
}

}

// Reaches the protected ordinary-object bodies the helpers below need.
struct DOMLegacyAccess {
// The named property visibility algorithm: P is a supported name, and nothing nearer than
// the named properties shadows it (an own property; with [LegacyOverrideBuiltIns], nothing does).
static bool is_named_properties_object(const Object* object) {
    if (object->get_type() != Object::ObjectType::Custom) return false;
    const auto* custom = static_cast<const CustomObjectBase*>(object);
    if (custom->get_custom_kind() != CustomObjectBase::CustomKind::Host) return false;
    const DOMTypeInfo* info = static_cast<const DOMObject*>(object)->type_;
    return info && info->legacy && info->legacy->subject;
}

static bool named_visible(const DOMObject* self, const DOMLegacyHooks& h, Context& ctx, const std::string& key, Value* out) {
    Value found;
    if (!h.named_get(ctx, const_cast<DOMObject*>(self), key, out ? out : &found)) return false;
    if (h.subject) {
        // A named properties object: judged against the global it answers for.
        Object* global = h.subject(const_cast<DOMObject*>(self));
        if (!global) return false;
        if (global->has_own_property(key)) return false;
        for (Object* proto = global->get_prototype(); proto; proto = proto->get_prototype()) {
            if (!is_named_properties_object(proto) && proto->has_own_property(key)) return false;
        }
        return true;
    }
    if (self->has_own_property_default(key)) return false;
    if (h.override_builtins) return true;
    for (Object* proto = self->get_prototype(); proto; proto = proto->get_prototype()) {
        if (proto->has_own_property(key)) return false;
    }
    return true;
}

// The names a legacy platform object reports as its own, for the key lists.
static std::vector<std::string> visible_named_keys(const DOMObject* self, const DOMLegacyHooks& h, Context& ctx) {
    std::vector<std::string> names;
    if (!h.named_keys) return names;
    if (h.subject) {
        for (std::string& name : h.named_keys(ctx, const_cast<DOMObject*>(self))) {
            if (named_visible(self, h, ctx, name, nullptr)) names.push_back(std::move(name));
        }
        return names;
    }
    for (std::string& name : h.named_keys(ctx, const_cast<DOMObject*>(self))) {
        if (self->has_own_property_default(name)) continue;
        bool shadowed = false;
        if (!h.override_builtins) {
            for (Object* proto = self->get_prototype(); proto && !shadowed; proto = proto->get_prototype()) {
                shadowed = proto->has_own_property(name);
            }
        }
        if (!shadowed) names.push_back(std::move(name));
    }
    return names;
}

// LegacyPlatformObjectGetOwnProperty. `handled` is set when an indexed or named property
// answered; otherwise the ordinary own property (if any) is what is returned.
static bool legacy_own(const DOMObject* self, const DOMLegacyHooks& h, Context& ctx, const std::string& key,
                       bool ignore_named, PropertyDescriptor& out) {
    uint32_t index = 0;
    const bool symbol = is_symbol_key(key);
    const bool is_index = !symbol && array_index(key, index);
    if (h.indexed_get && is_index) {
        Value value;
        if (h.indexed_get(ctx, const_cast<DOMObject*>(self), index, &value)) {
            out = PropertyDescriptor(value, static_cast<PropertyAttributes>(
                PropertyAttributes::Enumerable | PropertyAttributes::Configurable |
                (h.indexed_set ? PropertyAttributes::Writable : 0)));
            return true;
        }
        ignore_named = true;
    }
    if (h.named_get && !ignore_named && !symbol) {
        Value value;
        if (named_visible(self, h, ctx, key, &value)) {
            out = PropertyDescriptor(value, static_cast<PropertyAttributes>(
                PropertyAttributes::Configurable |
                (h.unenumerable_named ? 0 : PropertyAttributes::Enumerable) |
                (h.named_set || h.subject ? PropertyAttributes::Writable : 0)));
            return true;
        }
    }
    if (self->has_own_property_default(key)) {
        out = self->get_property_descriptor_default(key);
        return true;
    }
    return false;
}

};

#define QUANTA_LEGACY_PROLOGUE(fallback)                                                         \
    const DOMLegacyHooks* hooks = type_ ? type_->legacy : nullptr;                               \
    Context* ctx = hooks ? hook_context() : nullptr;                                             \
    if (!hooks || !ctx) return fallback;

bool DOMObject::legacy_has_own_property(const std::string& key) const {
    QUANTA_LEGACY_PROLOGUE(has_own_property_default(key))
    PropertyDescriptor desc;
    return DOMLegacyAccess::legacy_own(this, *hooks, *ctx, key, false, desc);
}

bool DOMObject::legacy_has_property(const std::string& key) const {
    QUANTA_LEGACY_PROLOGUE(has_property_default(key))
    uint32_t index = 0;
    if (hooks->indexed_get && !is_symbol_key(key) && array_index(key, index)) {
        Value value;
        return hooks->indexed_get(*ctx, const_cast<DOMObject*>(this), index, &value);
    }
    PropertyDescriptor desc;
    if (DOMLegacyAccess::legacy_own(this, *hooks, *ctx, key, false, desc)) return true;
    return has_property_default(key);
}

Value DOMObject::legacy_get_property(const std::string& key) const {
    QUANTA_LEGACY_PROLOGUE(get_property_default(key))
    if (is_symbol_key(key)) return get_property_default(key);
    uint32_t index = 0;
    bool ignore_named = false;
    if (hooks->indexed_get && array_index(key, index)) {
        Value value;
        if (hooks->indexed_get(*ctx, const_cast<DOMObject*>(this), index, &value)) return value;
        ignore_named = true;
    }
    if (hooks->named_get && !ignore_named) {
        Value value;
        if (DOMLegacyAccess::named_visible(this, *hooks, *ctx, key, &value)) return value;
    }
    return get_property_default(key);
}

bool DOMObject::legacy_set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    QUANTA_LEGACY_PROLOGUE(set_property_default(key, value, attrs))
    if (hooks->subject) return false;   // a named properties object takes no property of its own
    if (is_symbol_key(key)) return set_property_default(key, value, attrs);
    uint32_t index = 0;
    if ((hooks->indexed_get || hooks->indexed_set) && array_index(key, index)) {
        if (!hooks->indexed_set) return false;
        hooks->indexed_set(*ctx, this, index, value);
        return !ctx->has_exception();
    }
    if (hooks->named_set) {
        hooks->named_set(*ctx, this, key, value);
        return !ctx->has_exception();
    }
    // A named property with no setter cannot be replaced by an ordinary one.
    if (hooks->named_get && DOMLegacyAccess::named_visible(this, *hooks, *ctx, key, nullptr)) return false;
    return set_property_default(key, value, attrs);
}

bool DOMObject::legacy_delete_property(const std::string& key) {
    QUANTA_LEGACY_PROLOGUE(delete_property_default(key))
    if (hooks->subject) return false;
    if (is_symbol_key(key)) return delete_property_default(key);
    uint32_t index = 0;
    if (hooks->indexed_get && array_index(key, index)) {
        Value value;
        if (!hooks->indexed_get(*ctx, this, index, &value)) return true;
        if (!hooks->indexed_delete) return false;
        return hooks->indexed_delete(*ctx, this, index);
    }
    if (hooks->named_get && DOMLegacyAccess::named_visible(this, *hooks, *ctx, key, nullptr)) {
        if (!hooks->named_delete) return false;
        return hooks->named_delete(*ctx, this, key);
    }
    return delete_property_default(key);
}

std::vector<std::string> DOMObject::legacy_get_own_property_keys() const {
    QUANTA_LEGACY_PROLOGUE(get_own_property_keys_default())
    std::vector<std::string> keys;
    if (hooks->indexed_get && hooks->indexed_length) {
        const uint32_t length = hooks->indexed_length(*ctx, const_cast<DOMObject*>(this));
        for (uint32_t i = 0; i < length; i++) keys.push_back(std::to_string(i));
    }
    for (std::string& name : DOMLegacyAccess::visible_named_keys(this, *hooks, *ctx)) keys.push_back(std::move(name));
    for (std::string& key : get_own_property_keys_default()) keys.push_back(std::move(key));
    return keys;
}

std::vector<std::string> DOMObject::legacy_get_enumerable_keys() const {
    QUANTA_LEGACY_PROLOGUE(get_enumerable_keys_default())
    // EnumerableOwnProperties: the own keys, asked one by one whether they are enumerable.
    std::vector<std::string> keys;
    for (std::string& key : legacy_get_own_property_keys()) {
        if (is_symbol_key(key)) continue;
        PropertyDescriptor desc;
        if (DOMLegacyAccess::legacy_own(this, *hooks, *ctx, key, false, desc) && desc.is_enumerable()) {
            keys.push_back(std::move(key));
        }
    }
    return keys;
}

PropertyDescriptor DOMObject::legacy_get_property_descriptor(const std::string& key) const {
    QUANTA_LEGACY_PROLOGUE(get_property_descriptor_default(key))
    PropertyDescriptor desc;
    if (DOMLegacyAccess::legacy_own(this, *hooks, *ctx, key, false, desc)) return desc;
    return get_property_descriptor_default(key);
}

bool DOMObject::legacy_set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
    QUANTA_LEGACY_PROLOGUE(set_property_descriptor_default(key, desc))
    if (hooks->subject) return false;
    if (is_symbol_key(key)) return set_property_descriptor_default(key, desc);
    uint32_t index = 0;
    if ((hooks->indexed_get || hooks->indexed_set) && array_index(key, index)) {
        if (!hooks->indexed_set || desc.is_accessor_descriptor()) return false;
        hooks->indexed_set(*ctx, this, index, desc.get_value());
        return !ctx->has_exception();
    }
    if (hooks->named_get || hooks->named_set) {
        const bool supported = hooks->named_get && DOMLegacyAccess::named_visible(this, *hooks, *ctx, key, nullptr);
        const bool has_own = has_own_property_default(key);
        if (hooks->override_builtins || !has_own) {
            if (supported && !hooks->named_set) return false;
            if (hooks->named_set) {
                if (desc.is_accessor_descriptor()) return false;
                hooks->named_set(*ctx, this, key, desc.get_value());
                return !ctx->has_exception();
            }
        }
    }
    return set_property_descriptor_default(key, desc);
}

#undef QUANTA_LEGACY_PROLOGUE

bool DOMObject::rejects_prevent_extensions(const Object* object) {
    if (!object || object->get_type() != Object::ObjectType::Custom) return false;
    if (static_cast<const CustomObjectBase*>(object)->get_custom_kind() != CustomObjectBase::CustomKind::Host) return false;
    const DOMTypeInfo* info = static_cast<const DOMObject*>(object)->type_;
    return info && info->legacy;
}

}
