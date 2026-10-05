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
#include <memory>
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
//   static Object* NamedPropertiesSubject(T&);   // makes the object a named properties object, see below
//
// A named properties object is what sits in a global's prototype chain (Window.prototype -> the named
// properties object -> EventTarget.prototype) and answers for the global's named properties. Its
// NamedPropertiesSubject is that global: the visibility of a name is judged against the global (not shadowed
// by one of its own properties or by any prototype above it that is not itself a named properties object), the
// properties it reports are writable and configurable, and it refuses [[DefineOwnProperty]], [[Delete]] and
// [[PreventExtensions]] (and keeps its prototype).
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
    Object* (*subject)(DOMObject*) = nullptr;
};

// The internal methods of an exotic host object: one that decides for itself what each property operation
// means (WindowProxy and the cross-origin Window and Location). A host type declares the ones it takes over as
// static member functions; an operation it leaves out is the ordinary one of the object itself. They run on
// the context of whoever is running, so an error they raise belongs to the caller's realm.
//
//   static bool ExoticGetOwnProperty(Context&, T&, const std::string& key, PropertyDescriptor& out);  // false: none
//   static bool ExoticDefineOwnProperty(Context&, T&, const std::string& key, const PropertyDescriptor&);
//   static bool ExoticHasProperty(Context&, T&, const std::string& key);
//   static Value ExoticGet(Context&, T&, const std::string& key);
//   static bool ExoticSet(Context&, T&, const std::string& key, const Value&);   // false: the [[Set]] failed
//   static bool ExoticDelete(Context&, T&, const std::string& key);
//   static std::vector<std::string> ExoticOwnKeys(Context&, T&);
//   static bool ExoticGetPrototypeOf(Context&, T&, Object*& out);   // out: null for no prototype
//   static bool ExoticSetPrototypeOf(Context&, T&, Object* prototype);
//   static bool ExoticIsExtensible(Context&, T&);
//   static bool ExoticPreventExtensions(Context&, T&);
struct DOMExoticHooks {
    bool (*get_own_property)(Context&, DOMObject*, const std::string&, PropertyDescriptor&) = nullptr;
    bool (*define_own_property)(Context&, DOMObject*, const std::string&, const PropertyDescriptor&) = nullptr;
    bool (*has_property)(Context&, DOMObject*, const std::string&) = nullptr;
    Value (*get)(Context&, DOMObject*, const std::string&) = nullptr;
    bool (*set)(Context&, DOMObject*, const std::string&, const Value&) = nullptr;
    bool (*remove)(Context&, DOMObject*, const std::string&) = nullptr;
    std::vector<std::string> (*own_keys)(Context&, DOMObject*) = nullptr;
    bool (*get_prototype_of)(Context&, DOMObject*, Object*&) = nullptr;
    bool (*set_prototype_of)(Context&, DOMObject*, Object*) = nullptr;
    bool (*is_extensible)(Context&, DOMObject*) = nullptr;
    bool (*prevent_extensions)(Context&, DOMObject*) = nullptr;
};

// What the collector needs to know about one concrete DOMObject type: how to
// report the cells its C++ members reference, and how to destroy it.
struct DOMTypeInfo {
    const DOMTypeInfo* parent;
    void (*visit)(DOMObject*, Visitor&);
    void (*destroy)(DOMObject*);
    const DOMLegacyHooks* legacy;
    void (*finalize)(DOMObject*);
    const DOMExoticHooks* exotic;
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
    if constexpr (requires(T& t) { { T::NamedPropertiesSubject(t) } -> std::convertible_to<Object*>; }) {
        h.subject = [](DOMObject* o) -> Object* { return T::NamedPropertiesSubject(*static_cast<T*>(o)); };
    }
    if constexpr (requires { T::LegacyOverrideBuiltIns; }) h.override_builtins = T::LegacyOverrideBuiltIns;
    if constexpr (requires { T::LegacyUnenumerableNamedProperties; }) h.unenumerable_named = T::LegacyUnenumerableNamedProperties;
    return h;
}

template <class T>
inline constexpr DOMLegacyHooks dom_legacy_hooks_of = dom_make_legacy_hooks<T>();

template <class T>
constexpr bool dom_has_exotic_hooks() {
    return requires { T::ExoticGetOwnProperty; } || requires { T::ExoticGet; } || requires { T::ExoticGetPrototypeOf; };
}

template <class T>
constexpr DOMExoticHooks dom_make_exotic_hooks() {
    DOMExoticHooks h;
    if constexpr (requires(Context& c, T& t, const std::string& k, PropertyDescriptor& d) { { T::ExoticGetOwnProperty(c, t, k, d) } -> std::convertible_to<bool>; }) {
        h.get_own_property = [](Context& c, DOMObject* o, const std::string& k, PropertyDescriptor& d) -> bool { return T::ExoticGetOwnProperty(c, *static_cast<T*>(o), k, d); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& k, const PropertyDescriptor& d) { { T::ExoticDefineOwnProperty(c, t, k, d) } -> std::convertible_to<bool>; }) {
        h.define_own_property = [](Context& c, DOMObject* o, const std::string& k, const PropertyDescriptor& d) -> bool { return T::ExoticDefineOwnProperty(c, *static_cast<T*>(o), k, d); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& k) { { T::ExoticHasProperty(c, t, k) } -> std::convertible_to<bool>; }) {
        h.has_property = [](Context& c, DOMObject* o, const std::string& k) -> bool { return T::ExoticHasProperty(c, *static_cast<T*>(o), k); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& k) { { T::ExoticGet(c, t, k) } -> std::convertible_to<Value>; }) {
        h.get = [](Context& c, DOMObject* o, const std::string& k) -> Value { return T::ExoticGet(c, *static_cast<T*>(o), k); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& k, const Value& v) { { T::ExoticSet(c, t, k, v) } -> std::convertible_to<bool>; }) {
        h.set = [](Context& c, DOMObject* o, const std::string& k, const Value& v) -> bool { return T::ExoticSet(c, *static_cast<T*>(o), k, v); };
    }
    if constexpr (requires(Context& c, T& t, const std::string& k) { { T::ExoticDelete(c, t, k) } -> std::convertible_to<bool>; }) {
        h.remove = [](Context& c, DOMObject* o, const std::string& k) -> bool { return T::ExoticDelete(c, *static_cast<T*>(o), k); };
    }
    if constexpr (requires(Context& c, T& t) { { T::ExoticOwnKeys(c, t) } -> std::convertible_to<std::vector<std::string>>; }) {
        h.own_keys = [](Context& c, DOMObject* o) -> std::vector<std::string> { return T::ExoticOwnKeys(c, *static_cast<T*>(o)); };
    }
    if constexpr (requires(Context& c, T& t, Object*& out) { { T::ExoticGetPrototypeOf(c, t, out) } -> std::convertible_to<bool>; }) {
        h.get_prototype_of = [](Context& c, DOMObject* o, Object*& out) -> bool { return T::ExoticGetPrototypeOf(c, *static_cast<T*>(o), out); };
    }
    if constexpr (requires(Context& c, T& t, Object* p) { { T::ExoticSetPrototypeOf(c, t, p) } -> std::convertible_to<bool>; }) {
        h.set_prototype_of = [](Context& c, DOMObject* o, Object* p) -> bool { return T::ExoticSetPrototypeOf(c, *static_cast<T*>(o), p); };
    }
    if constexpr (requires(Context& c, T& t) { { T::ExoticIsExtensible(c, t) } -> std::convertible_to<bool>; }) {
        h.is_extensible = [](Context& c, DOMObject* o) -> bool { return T::ExoticIsExtensible(c, *static_cast<T*>(o)); };
    }
    if constexpr (requires(Context& c, T& t) { { T::ExoticPreventExtensions(c, t) } -> std::convertible_to<bool>; }) {
        h.prevent_extensions = [](Context& c, DOMObject* o) -> bool { return T::ExoticPreventExtensions(c, *static_cast<T*>(o)); };
    }
    return h;
}

template <class T>
inline constexpr DOMExoticHooks dom_exotic_hooks_of = dom_make_exotic_hooks<T>();

// A type declares `void Finalize()` to hear that it is dead before anything is destroyed.
template <class T>
constexpr void (*dom_finalizer_of())(DOMObject*) {
    if constexpr (requires(T& t) { t.Finalize(); }) {
        return [](DOMObject* o) { static_cast<T*>(o)->Finalize(); };
    } else {
        return nullptr;
    }
}

template <class T>
struct DOMTypeOf {
    static constexpr DOMTypeInfo info = {
        dom_parent_info<T>(),
        [](DOMObject* o, Visitor& v) { static_cast<T*>(o)->Visit(v); },
        [](DOMObject* o) { static_cast<T*>(o)->~T(); },
        dom_has_legacy_hooks<T>() ? &dom_legacy_hooks_of<T> : nullptr,
        dom_finalizer_of<T>(),
        dom_has_exotic_hooks<T>() ? &dom_exotic_hooks_of<T> : nullptr,
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
class TracedBase;

class DOMObject : public CustomObjectBase {
    const DOMTypeInfo* type_ = nullptr;
    // The Traced members this object declares, linked as they were constructed.
    TracedBase* traced_ = nullptr;

public:
    DOMObject();
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

    // Before any cell of a collection's dead is destroyed, the dead host objects that declared
    // `Finalize()` are told (and the WeakHandles that named a dead cell are cleared). Everything
    // is still intact then, so a finalizer may read other cells; see the finalization contract
    // in docs/embedding/native-objects.md.
    static bool finalization_pending();
    static void finalize_dead(const std::vector<Heap::DeadCell>& dead);

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
    // [[PreventExtensions]] of a legacy platform object (and of a named properties object) answers false.
    static bool rejects_prevent_extensions(const Object* object);

    // The prototype and extensibility internal methods of an exotic host object, for the callers that must see
    // them (Object.getPrototypeOf and friends). Each returns false when `object` is not exotic in that respect,
    // and the ordinary behaviour applies; otherwise `out` / `result` is the answer (an exception may be pending).
    static bool exotic_get_prototype_of(Object* object, Object*& out);
    static bool exotic_set_prototype_of(Object* object, Object* prototype, bool& result);
    static bool exotic_is_extensible(Object* object, bool& result);
    static bool exotic_prevent_extensions(Object* object, bool& result);
    static const DOMExoticHooks* exotic_of(const Object* object);

    // The ordinary property operations of the object itself, for an exotic object that answers for some of its
    // properties and leaves the rest to the plain object it also is.
    bool ordinary_has_property(const std::string& key) const;
    bool ordinary_has_own_property(const std::string& key) const;
    Value ordinary_get(const std::string& key) const;
    bool ordinary_set(const std::string& key, const Value& value);
    bool ordinary_delete(const std::string& key);
    std::vector<std::string> ordinary_own_keys() const;
    PropertyDescriptor ordinary_get_own_property(const std::string& key) const;
    bool ordinary_define_own_property(const std::string& key, const PropertyDescriptor& desc);

private:
    template <class T, class... A>
    friend T* Heap::Allocate(A&&...);
    friend class TracedBase;
    void set_dom_type(const DOMTypeInfo* info) { type_ = info; }
    static DOMObject* constructing();
    static void set_constructing(DOMObject* object);
    static void note_finalizable();

    bool is_a(const DOMTypeInfo* wanted) const {
        for (const DOMTypeInfo* t = type_; t; t = t->parent) {
            if (t == wanted) return true;
        }
        return false;
    }
};

// A member of a host object that holds cells, which the object traces by itself and writes through
// the barrier by itself: no Visit() entry to forget and no NoteWrite() to miss. A Traced member is
// declared in the object's own class and constructed with it (it finds the object being built); one
// made anywhere else is a programming error and aborts.
class TracedBase {
public:
    virtual ~TracedBase() = default;
    TracedBase(const TracedBase&) = delete;
    TracedBase& operator=(const TracedBase&) = delete;

protected:
    TracedBase();
    DOMObject* owner_;
    // After a cell reference was stored: the barrier an old owner needs for a young target.
    void NoteStored(const Value& value);

private:
    friend class DOMObject;
    virtual void Trace(Visitor& v) const = 0;
    TracedBase* next_ = nullptr;
};

// One value.
class TracedValue : public TracedBase {
public:
    TracedValue() = default;
    const Value& Get() const { return value_; }
    void Set(const Value& value) { value_ = value; NoteStored(value); }
    void Clear() { value_ = Value(); }

private:
    void Trace(Visitor& v) const override { v.visit(value_); }
    Value value_;
};

// A list of values: what a host object keeps instead of a std::vector<Value> it has to Mark by hand.
class TracedList : public TracedBase {
public:
    TracedList() = default;
    size_t size() const { return values_.size(); }
    bool empty() const { return values_.empty(); }
    const Value& operator[](size_t i) const { return values_[i]; }
    const Value* begin() const { return values_.data(); }
    const Value* end() const { return values_.data() + values_.size(); }
    void push_back(const Value& value) { values_.push_back(value); NoteStored(value); }
    void set(size_t i, const Value& value) { values_[i] = value; NoteStored(value); }
    void pop_back() { values_.pop_back(); }
    void erase(size_t i) { values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(i)); }
    void clear() { values_.clear(); }

private:
    void Trace(Visitor& v) const override { for (const Value& value : values_) v.visit(value); }
    std::vector<Value> values_;
};

// A reference to a cell that does not keep it alive: Get() returns the object while it lives and null
// once the collector has found it dead (the clearing happens before any destructor of that collection
// runs). For a host's own bookkeeping that must not extend a lifetime. Copies share the answer. A
// thread's handles belong to that thread.
class WeakHandleBase {
public:
    WeakHandleBase() = default;
    bool IsEmpty() const { return !Raw(); }
    void Reset() { slot_.reset(); }

protected:
    explicit WeakHandleBase(Object* target);
    Object* Raw() const { return slot_ ? slot_->target : nullptr; }

private:
    friend class DOMObject;
public:
    struct Slot { Object* target; };
private:
    std::shared_ptr<Slot> slot_;
};

template <class T>
class WeakHandle : public WeakHandleBase {
    static_assert(std::is_base_of_v<Object, T>, "a WeakHandle names a cell");

public:
    WeakHandle() = default;
    explicit WeakHandle(T* target) : WeakHandleBase(target) {}
    T* Get() const { return static_cast<T*>(Raw()); }
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
    DOMObject* const outer = DOMObject::constructing();
    T* obj = new T(std::forward<A>(args)...);
    DOMObject::set_constructing(outer);
    obj->set_dom_type(&DOMTypeOf<T>::info);
    if constexpr (DOMTypeOf<T>::info.finalize != nullptr) DOMObject::note_finalizable();
    return obj;
}

}

#endif
