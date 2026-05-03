/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/BooleanBuiltin.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

namespace Quanta {

void register_boolean_builtins(Context& ctx) {
    auto boolean_constructor = ObjectFactory::create_native_constructor("Boolean",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool value = args.empty() ? false : args[0].to_boolean();

            // If this_obj exists (constructor call), set [[PrimitiveValue]]
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("[[PrimitiveValue]]", Value(value));
            }

            // Always return primitive boolean
            // Function::construct will return the created object if called as constructor
            return Value(value);
        });

    auto boolean_prototype = ObjectFactory::create_object();

    auto boolean_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return this_val;
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.valueOf called on non-boolean")));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.valueOf called on non-boolean")));
                return Value();
            }
        }, 0);

    PropertyDescriptor boolean_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
    boolean_valueOf_name_desc.set_configurable(true);
    boolean_valueOf_name_desc.set_enumerable(false);
    boolean_valueOf_name_desc.set_writable(false);
    boolean_valueOf->set_property_descriptor("name", boolean_valueOf_name_desc);

    PropertyDescriptor boolean_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
    boolean_valueOf_length_desc.set_enumerable(false);
    boolean_valueOf_length_desc.set_writable(false);
    boolean_valueOf->set_property_descriptor("length", boolean_valueOf_length_desc);

    auto boolean_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return Value(this_val.to_boolean() ? "true" : "false");
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        return Value(primitive.to_boolean() ? "true" : "false");
                    }
                }
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.toString called on non-boolean")));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.toString called on non-boolean")));
                return Value();
            }
        }, 0);

    PropertyDescriptor boolean_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    boolean_toString_name_desc.set_configurable(true);
    boolean_toString_name_desc.set_enumerable(false);
    boolean_toString_name_desc.set_writable(false);
    boolean_toString->set_property_descriptor("name", boolean_toString_name_desc);

    PropertyDescriptor boolean_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    boolean_toString_length_desc.set_enumerable(false);
    boolean_toString_length_desc.set_writable(false);
    boolean_toString->set_property_descriptor("length", boolean_toString_length_desc);

    PropertyDescriptor boolean_valueOf_desc(Value(boolean_valueOf.release()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("valueOf", boolean_valueOf_desc);
    PropertyDescriptor boolean_toString_desc(Value(boolean_toString.release()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("toString", boolean_toString_desc);
    PropertyDescriptor boolean_constructor_desc(Value(boolean_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("constructor", boolean_constructor_desc);

    boolean_constructor->set_property("prototype", Value(boolean_prototype.release()));

    ctx.register_built_in_object("Boolean", boolean_constructor.release());
}

} // namespace Quanta
