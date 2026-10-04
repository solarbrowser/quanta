/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_DOMOBJECT_H
#define QUANTA_RUNTIME_DOMOBJECT_H

#include "quanta/core/gc/Heap.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Value.h"
#include <concepts>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Quanta {

class Context;
class DOMObject;

// The hooks of a Web IDL legacy platform object: one that answers `list[0]` and
// `collection["id"]` itself. A host type declares the ones it supports as static member
// functions (found by name when the type is allocated), and the engine gives the object
// the property semantics of WebIDL's [[GetOwnProperty]], [[Set]], [[DefineOwnProperty]],
// [[Delete]] and [[OwnPropertyKeys]] for legacy platform objects:
//
//   static bool IndexedGetter(Context&, T&, uint32_t index, Value& out);   // false: no such index
//   static void IndexedSetter(Context&, T&, uint32_t index, const Value&); // absent: read-only
//   static bool IndexedDeleter(Context&, T&, uint32_t index);              // true: deleted
//   static uint32_t IndexedLength(Context&, T&);   // the supported indices are 0 .. length-1
//   static bool NamedGetter(Context&, T&, const std::string& name, Value& out);
//   static void NamedSetter(Context&, T&, const std::string& name, const Value&);
//   static bool NamedDeleter(Context&, T&, const std::string& name);
//   static std::vector<std::string> NamedKeys(Context&, T&);   // the supported names
//   static constexpr bool LegacyOverrideBuiltIns = true;                // [LegacyOverrideBuiltIns]
//   static constexpr bool LegacyUnenumerableNamedProperties = true;     // [LegacyUnenumerableNamedProperties]
//
// An exception a hook raises is reported through the context's flag, as in any native.
struct DOMLegacyHooks {
    bool (*indexed_get)(Context&, DOMObject*, uint32_t, Value*) = nullptr;
    void (*indexed_set)(Context&, DOMObject*, uint32_t, const Value&) = nullptr;
    bool (*indexed_delete)(Context&, DOMObject*, uint32_t) = nullptr;
    uint32_t (*indexed_length)(Context&, DOMObject*) = nullptr;
    bool (*named_get)(Context&, DOMObject*, const std::string&, Value*) = nullptr;
    void (*named_set)(Context&, DOMObject*, const std::string&, const Value&) = nullptr;
    bool (*named_delete)(Context&, DOMObject*, const std::string&) = nullptr;
    std::vector<std::string> (*named_keys)(Context&, DOMObject*) = nullptr;
    bool override_builtins = false;
    bool unenumerable_named = false;
};

// What the collector needs to know about one concrete DOMObject type: how to
// report the cells its C++ members reference, and how to destroy it.
struct DOMTypeInfo {
    const DOMTypeInfo* parent;
    void (*visit)(DOMObject*, Visitor&);
    void (*destroy)(DOMObject*);
    const DOMLegacyHooks* legacy;
};

template <class T>
struct DOMTypeOf;

// A host type declares `using Parent = Base;` to make Cast<Base> succeed on
// its instances. Without it the type is only castable to itself. Reached
// through DOMTypeOf<Parent> rather than a variable template of its own so the
// two can refer to each other before either is defined.
template <class T>
constexpr const DOMTypeInfo* dom_parent_info() {
    if constexpr (requires { typename T::Parent; }) {
        return &DOMTypeOf<typename T::Parent>::info;
    } else {
        return nullptr;
    }
}

// One table per concrete type, built from T's own Visit() and destructor. This
// stands in for virtual dispatch: Object carries no vtable (a vptr ahead of the
// Object subobject would put the cell's base address and its Object* apart,
// and every trace edge, write barrier and conservative probe assumes they are
// the same word), so a polymorphic host type cannot be had by deriving from
// Object and adding `virtual`. A function-pointer table reachable from a field
// gives the same open-ended dispatch without moving anything. Constant
// initialized, so there is no start-up order to get wrong.
template <class T>
constexpr bool dom_has_legacy_hooks() {
    return requires { T::IndexedGetter; } || requires { T::NamedGetter; };
}

template <class T>
constexpr DOMLegacyHooks dom_make_legacy_hooks() {
    DOMLegacyHooks h;
    if constexpr (requires(Context& c, T& t, uint32_t i, Value& v) { { T::IndexedGetter(c, t, i, v) } -> std::convertible_to<bool>; }) {
        h.indexed_get = [](Context& c, DOMObject* o, uint32_t i, Value* out) -> bool {
            return T::IndexedGetter(c, *static_cast<T*>(o), i, *out);
        };
    }
    if constexpr (requires(Context& c, T& t, uint32_t i, const Value& v) { T::IndexedSetter(c, t, i, v); }) {
        h.indexed_set = [](Context& c, DOMObject* o, uint32_t i, const Value& v) {
            T::IndexedSetter(c, *static_cast<T*>(o), i, v);
        };
    }
    if constexpr (requires(Context& c, T& t, uint32_t i) { { T::IndexedDeleter(c, t, i) } -> std::convertible_to<bool>; }) {
        h.indexed_delete = [](Context& c, DOMObject* o, uint32_t i) -> bool {
            return T::IndexedDeleter(c, *static_cast<T*>(o), i);
        };
    }
    if constexpr (requires(Context& c, T& t) { { T::IndexedLength(c, t) } -> std::convertible_to<uint32_t>; }) {
        h.indexed_length = [](Context& c, DOMObject* o) -> uint32_t { return T::IndexedLength(c, *static_cast<T*>(o)); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& n, Value& v) { { T::NamedGetter(c, t, n, v) } -> std::convertible_to<bool>; }) {
        h.named_get = [](Context& c, DOMObject* o, const std::string& n, Value* out) -> bool {
            return T::NamedGetter(c, *static_cast<T*>(o), n, *out);
        };
    }
    if constexpr (requires(Context& c, T& t, const std::string& n, const Value& v) { T::NamedSetter(c, t, n, v); }) {
        h.named_set = [](Context& c, DOMObject* o, const std::string& n, const Value& v) {
            T::NamedSetter(c, *static_cast<T*>(o), n, v);
        };
    }
    if constexpr (requires(Context& c, T& t, const std::string& n) { { T::NamedDeleter(c, t, n) } -> std::convertible_to<bool>; }) {
        h.named_delete = [](Context& c, DOMObject* o, const std::string& n) -> bool {
            return T::NamedDeleter(c, *static_cast<T*>(o), n);
        };
    }
    if constexpr (requires(Context& c, T& t) { { T::NamedKeys(c, t) } -> std::convertible_to<std::vector<std::string>>; }) {
        h.named_keys = [](Context& c, DOMObject* o) -> std::vector<std::string> { return T::NamedKeys(c, *static_cast<T*>(o)); };
    }
    if constexpr (requires { T::LegacyOverrideBuiltIns; }) h.override_builtins = T::LegacyOverrideBuiltIns;
    if constexpr (requires { T::LegacyUnenumerableNamedProperties; }) h.unenumerable_named = T::LegacyUnenumerableNamedProperties;
    return h;
}

template <class T>
inline constexpr DOMLegacyHooks dom_legacy_hooks_of = dom_make_legacy_hooks<T>();

template <class T>
struct DOMTypeOf {
    static constexpr DOMTypeInfo info = {
        dom_parent_info<T>(),
        [](DOMObject* o, Visitor& v) { static_cast<T*>(o)->Visit(v); },
        [](DOMObject* o) { static_cast<T*>(o)->~T(); },
        dom_has_legacy_hooks<T>() ? &dom_legacy_hooks_of<T> : nullptr,
    };
};

// Base of every native object an embedder hands to script. Derive, give the
// derived type a non-virtual `void Visit(Visitor&)` that Mark()s every cell its
// C++ members point at, and create instances with Heap::Allocate<T>().
//
// Guarantees:
//  - The cell never moves.
//  - The destructor runs when the cell is swept, so std::string, std::vector
//    and the like are fine as members.
//  - Visit() runs whenever the collector traces the object, and is the ONLY
//    way a cell referenced from a C++ member stays alive.
class DOMObject : public CustomObjectBase {
    const DOMTypeInfo* type_ = nullptr;

public:
    DOMObject() : CustomObjectBase(ObjectType::Custom) { set_custom_kind(CustomKind::Host); }
    ~DOMObject() = default;

    // The base reports nothing; a derived type's own Visit() hides this one.
    void Visit(Visitor&) {}

    // `v` as a T, or nullptr when it is not a host object of that type (or of a
    // type declaring T as an ancestor through `Parent`).
    template <class T>
    static T* Cast(const Value& v);

    // Call after storing a cell reference into a member Visit() reports, once
    // the object has been through a collection. A young object is traced in
    // full by the next minor anyway, but an old one is not, and without this
    // the new edge is invisible to it: the target is swept while the object
    // still points at it. The Value form shades only the new target, which is
    // the cheaper of the two when the object reports many edges.
    void NoteWrite();
    void NoteWrite(const Value& stored);

    // Collector entry points.
    void trace(Visitor& v);
    void destroy();

    friend struct DOMLegacyAccess;

    // The nine property operations of a legacy platform object (see DOMLegacyHooks), reached
    // from CustomObjectBase's own dispatch. For a type with no hooks each is the plain-Object body.
    bool legacy_has_property(const std::string& key) const;
    bool legacy_has_own_property(const std::string& key) const;
    Value legacy_get_property(const std::string& key) const;
    bool legacy_set_property(const std::string& key, const Value& value, PropertyAttributes attrs);
    bool legacy_delete_property(const std::string& key);
    std::vector<std::string> legacy_get_own_property_keys() const;
    std::vector<std::string> legacy_get_enumerable_keys() const;
    PropertyDescriptor legacy_get_property_descriptor(const std::string& key) const;
    bool legacy_set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);

private:
    template <class T, class... A>
    friend T* Heap::Allocate(A&&...);
    void set_dom_type(const DOMTypeInfo* info) { type_ = info; }

    bool is_a(const DOMTypeInfo* wanted) const {
        for (const DOMTypeInfo* t = type_; t; t = t->parent) {
            if (t == wanted) return true;
        }
        return false;
    }
};

template <class T>
T* DOMObject::Cast(const Value& v) {
    static_assert(std::is_base_of_v<DOMObject, T>, "Cast target must derive from DOMObject");
    if (!v.is_object()) return nullptr;
    Object* o = v.as_object();
    if (o->get_type() != Object::ObjectType::Custom) return nullptr;
    auto* base = static_cast<CustomObjectBase*>(o);
    if (base->get_custom_kind() != CustomObjectBase::CustomKind::Host) return nullptr;
    auto* dom = static_cast<DOMObject*>(base);
    if constexpr (std::is_same_v<T, DOMObject>) {
        return dom;
    } else {
        return dom->is_a(&DOMTypeOf<T>::info) ? static_cast<T*>(dom) : nullptr;
    }
}

template <class T, class... A>
T* Heap::Allocate(A&&... args) {
    static_assert(std::is_base_of_v<DOMObject, T>, "Heap::Allocate<T> needs T : DOMObject");
    static_assert(alignof(T) <= HeapBlock::kCellAlign, "cells are 16-byte aligned");
    // A cell past the size classes is a malloc'd large cell, and the first one
    // ever made switches every trace edge over to the slower path that can
    // resolve it (see g_any_large_cell). Not worth it for a host object.
    static_assert(sizeof(T) <= kMaxTier1Size, "host object too large for a heap block");
    T* obj = new T(std::forward<A>(args)...);
    obj->set_dom_type(&DOMTypeOf<T>::info);
    return obj;
}

}

#endif
