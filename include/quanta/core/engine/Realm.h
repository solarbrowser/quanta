/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_REALM_H
#define QUANTA_ENGINE_REALM_H

#include <cstddef>

namespace Quanta {

class Object;
class Function;
class Engine;
class Visitor;

// The intrinsics one realm owns. They used to be thread-wide statics, which made
// a second realm on the thread overwrite (or be overwritten by) the first; held
// here, each realm has its own, and the code that has no Context in hand -- the
// object factory, the interpreter's primitive property access -- finds them
// through the thread's current realm (g_current_realm, switched by RealmScope
// where execution enters a realm).
//
// Owned by the realm's Engine and reachable from its global Context
// (Context::realm()), which traces it, so nothing here needs a root of its own.
class Realm {
public:
    static constexpr size_t kPrimitiveKinds = 5;  // String, Number, Boolean, BigInt, Symbol

    explicit Realm(Engine* engine = nullptr) : engine_(engine) {}
    Realm(const Realm&) = delete;
    Realm& operator=(const Realm&) = delete;

    Engine* engine() const { return engine_; }

    // ObjectFactory's prototypes, and the exact Function.prototype.call/apply a
    // call site may skip invoking (see ObjectFactory::set_pristine_function_call).
    Object* object_proto = nullptr;
    Object* array_proto = nullptr;
    Object* function_proto = nullptr;
    Function* pristine_call = nullptr;
    Function* pristine_apply = nullptr;

    // %ThrowTypeError%: one per realm, shared by Function.prototype.caller/
    // .arguments and arguments.callee.
    Object* throw_type_error = nullptr;

    // %String.prototype% and friends, and %Promise%, captured once the builtins
    // are installed and still pristine (see Context::capture_primitive_prototypes).
    Object* primitive_protos[kPrimitiveKinds] = {};
    Function* intrinsic_promise = nullptr;

    // %IteratorPrototype% and the prototypes of the built-in iterators.
    Object* iterator_proto = nullptr;
    Object* array_iterator_proto = nullptr;
    Object* string_iterator_proto = nullptr;
    Object* map_iterator_proto = nullptr;
    Object* set_iterator_proto = nullptr;

    // %GeneratorPrototype%, %GeneratorFunction.prototype%, and the exact Function
    // installed as %GeneratorPrototype%.next.
    Object* generator_proto = nullptr;
    Object* generator_function_proto = nullptr;
    Function* generator_next_fn = nullptr;
    Object* async_generator_proto = nullptr;
    Object* async_generator_function_proto = nullptr;

    // The keyed collections' and weak references' prototypes.
    Object* map_proto = nullptr;
    Object* set_proto = nullptr;
    Object* weakmap_proto = nullptr;
    Object* weakset_proto = nullptr;
    Object* weakref_proto = nullptr;
    Object* finalization_registry_proto = nullptr;

    void trace(Visitor& v) const;

private:
    Engine* engine_;
};

// The realm the running code belongs to. Null before any engine exists.
inline constinit thread_local Realm* g_current_realm = nullptr;

// A realm with nothing in it, standing in where code asks for the current
// realm's intrinsics with no realm current (before the first engine, after the
// last): reads find nothing, as the statics this replaces did, and writes land
// somewhere harmless.
Realm& null_realm();

inline Realm& current_realm() {
    Realm* realm = g_current_realm;
    return realm ? *realm : null_realm();
}

// Makes `realm` current for a scope, restoring whatever was current before.
class RealmScope {
public:
    explicit RealmScope(Realm* realm) : previous_(g_current_realm) { g_current_realm = realm; }
    ~RealmScope() { g_current_realm = previous_; }

    // Leaves this scope's realm current once the scope ends, instead of restoring
    // the previous one.
    void commit() { previous_ = g_current_realm; }

    RealmScope(const RealmScope&) = delete;
    RealmScope& operator=(const RealmScope&) = delete;

private:
    Realm* previous_;
};

}

#endif
