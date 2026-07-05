/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_VISITOR_H
#define QUANTA_GC_VISITOR_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"

namespace Quanta {

class Context;
class Environment;
class WeakMap;
class WeakSet;
class WeakRef;
class FinalizationRegistry;

// Edge callback for the marking phase. `trace` implementations report every
// cell reference their object holds; the collector supplies the concrete
// visitor. Null pointers may be passed freely -- the visitor filters them.
class Visitor {
public:
    virtual ~Visitor() = default;

    virtual void visit_object(Object* o) = 0;
    virtual void visit_string(String* s) = 0;
    virtual void visit_symbol(Symbol* s) = 0;
    virtual void visit_bigint(BigInt* b) = 0;

    // Contexts and Environments are not cells, but they hold cell references
    // (binding tables, this/exception slots) and must be walked exactly once
    // per collection -- the visitor keeps the visited set.
    virtual void visit_context(Context* ctx) = 0;
    virtual void visit_environment(Environment* env) = 0;

    // Ephemeron participants: their weakly-held members are not ordinary
    // edges (a live WeakMap pointing at a dead key is not a bug), so trace()
    // reports them here instead of via visit_object/visit. Only the
    // collector's mark pass needs to act on these; every other visitor
    // (verify, etc.) uses the no-op default.
    virtual void visit_weak_map(WeakMap*) {}
    virtual void visit_weak_set(WeakSet*) {}
    virtual void visit_weak_ref(WeakRef*) {}
    virtual void visit_finalization_registry(FinalizationRegistry*) {}

    // NaN-box decode + dispatch.
    void visit(const Value& v) {
        if (v.is_object()) visit_object(v.as_object());
        else if (v.is_function()) visit_object(static_cast<Object*>(v.as_function()));
        else if (v.is_string()) visit_string(v.as_string());
        else if (v.is_symbol()) visit_symbol(v.as_symbol());
        else if (v.is_bigint()) visit_bigint(v.as_bigint());
    }
};

}

#endif
