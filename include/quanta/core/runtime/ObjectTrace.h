/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_OBJECTTRACE_H
#define QUANTA_RUNTIME_OBJECTTRACE_H

#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Shape.h"

namespace Quanta {

// Object::trace_default() with the per-edge virtual call taken out: the collector's mark
// loop walks millions of these and the visitor it hands them to is one concrete class.
// Keep the two in step -- this is the same walk.
template <class V>
inline void Object::trace_fast(V& v) {
    v.edge_object(proto_);
    // Only the logically valid extent of each region: the capacity beyond it is unused slack.
    const uint32_t elen = elements_length();
    // A numeric array is the common big one, and a per-element classify was most of what
    // marking it cost. Elements are kept as pure values with no flag saying "all numbers"
    // on purpose: that flag would have to be right at every store, including the ones the
    // JIT and the dense-slot fast paths make straight into the butterfly, and one missed
    // store is a cell the collector never sees. Testing four at a time needs no such fact.
    uint32_t i = 0;
    for (; i + 4 <= elen; i += 4) {
        const Value* e = element_ptr(i);
        if (!(e[0].may_hold_cell() | e[-1].may_hold_cell() | e[-2].may_hold_cell() | e[-3].may_hold_cell())) continue;
        v.edge_value(e[0]);
        v.edge_value(e[-1]);
        v.edge_value(e[-2]);
        v.edge_value(e[-3]);
    }
    for (; i < elen; i++) v.edge_value(*element_ptr(i));
    if (shape_) {
        const uint32_t scount = shape_->slot_count();
        for (uint32_t i = 0; i < scount; i++) v.edge_value(*shape_slot_ptr(i));
    }
    if (peek_extras()) trace_extras(v);
}

}  // namespace Quanta

#endif
