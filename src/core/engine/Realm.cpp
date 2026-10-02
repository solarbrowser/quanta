/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Realm.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/runtime/Object.h"

namespace Quanta {

void Realm::trace(Visitor& v) const {
    v.visit_object(object_proto);
    v.visit_object(array_proto);
    v.visit_object(function_proto);
    v.visit_object(pristine_call);
    v.visit_object(pristine_apply);
    v.visit_object(throw_type_error);
    for (Object* proto : primitive_protos) v.visit_object(proto);
    v.visit_object(intrinsic_promise);
}

}
