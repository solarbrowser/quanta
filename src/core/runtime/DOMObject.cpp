/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/DOMObject.h"
#include "quanta/core/gc/Collector.h"

namespace Quanta {

void DOMObject::trace(Visitor& v) {
    Object::trace_default(v);
    if (type_) type_->visit(this, v);
}

// A cell that was never given a type (its constructor threw before
// Heap::Allocate could stamp it) has only the base to destroy.
void DOMObject::destroy() {
    if (type_) type_->destroy(this);
    else this->~DOMObject();
}

void DOMObject::NoteWrite() {
    Collector::write_barrier(this);
}

void DOMObject::NoteWrite(const Value& stored) {
    Collector::write_barrier_value(this, stored);
}

}
