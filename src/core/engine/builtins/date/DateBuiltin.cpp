/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/DateBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Date.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_date_builtins(Context& ctx) {
    auto date_prototype = ObjectFactory::create_object();
    Object* date_proto_ptr = date_prototype.get();

    auto date_constructor_fn = ObjectFactory::create_native_constructor("Date",
        [date_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value result = Date::date_constructor(ctx, args);
            if (!result.is_object()) return result;

            // OrdinaryCreateFromConstructor: prototype comes from new.target,
            // falling back to %Date.prototype%.
            Object* proto = date_proto_ptr;
            Value nt = ctx.get_new_target();
            if (nt.is_object() || nt.is_function()) {
                Object* nt_obj = nt.is_function() ? static_cast<Object*>(nt.as_function())
                                                  : nt.as_object();
                Value p = nt_obj->get_property("prototype");
                if (ctx.has_exception()) return Value();
                if (p.is_object()) proto = p.as_object();
                else if (p.is_function()) proto = static_cast<Object*>(p.as_function());
            }
            result.as_object()->set_prototype(proto);
            return result;
        }, 7);

    struct Entry {
        const char* name;
        Value (*fn)(Context&, const std::vector<Value>&);
        uint32_t arity;
    };

    static const Entry statics[] = {
        {"now", Date::now, 0},
        {"parse", Date::parse, 1},
        {"UTC", Date::UTC, 7},
    };
    for (const Entry& e : statics) {
        auto fn = ObjectFactory::create_native_function(e.name, e.fn, e.arity);
        date_constructor_fn->set_property(e.name, Value(fn.release()), PropertyAttributes::BuiltinFunction);
    }

    static const Entry proto_methods[] = {
        {"getTime", Date::getTime, 0},
        {"getFullYear", Date::getFullYear, 0},
        {"getMonth", Date::getMonth, 0},
        {"getDate", Date::getDate, 0},
        {"getDay", Date::getDay, 0},
        {"getHours", Date::getHours, 0},
        {"getMinutes", Date::getMinutes, 0},
        {"getSeconds", Date::getSeconds, 0},
        {"getMilliseconds", Date::getMilliseconds, 0},
        {"getTimezoneOffset", Date::getTimezoneOffset, 0},
        {"getYear", Date::getYear, 0},
        {"getUTCFullYear", Date::getUTCFullYear, 0},
        {"getUTCMonth", Date::getUTCMonth, 0},
        {"getUTCDate", Date::getUTCDate, 0},
        {"getUTCDay", Date::getUTCDay, 0},
        {"getUTCHours", Date::getUTCHours, 0},
        {"getUTCMinutes", Date::getUTCMinutes, 0},
        {"getUTCSeconds", Date::getUTCSeconds, 0},
        {"getUTCMilliseconds", Date::getUTCMilliseconds, 0},
        {"setTime", Date::setTime, 1},
        {"setMilliseconds", Date::setMilliseconds, 1},
        {"setSeconds", Date::setSeconds, 2},
        {"setMinutes", Date::setMinutes, 3},
        {"setHours", Date::setHours, 4},
        {"setDate", Date::setDate, 1},
        {"setMonth", Date::setMonth, 2},
        {"setFullYear", Date::setFullYear, 3},
        {"setYear", Date::setYear, 1},
        {"setUTCMilliseconds", Date::setUTCMilliseconds, 1},
        {"setUTCSeconds", Date::setUTCSeconds, 2},
        {"setUTCMinutes", Date::setUTCMinutes, 3},
        {"setUTCHours", Date::setUTCHours, 4},
        {"setUTCDate", Date::setUTCDate, 1},
        {"setUTCMonth", Date::setUTCMonth, 2},
        {"setUTCFullYear", Date::setUTCFullYear, 3},
        {"toString", Date::toString, 0},
        {"toDateString", Date::toDateString, 0},
        {"toTimeString", Date::toTimeString, 0},
        {"toISOString", Date::toISOString, 0},
        {"toUTCString", Date::toUTCString, 0},
        {"toGMTString", Date::toGMTString, 0},
        {"toJSON", Date::toJSON, 1},
        {"toLocaleString", Date::toLocaleString, 0},
        {"toLocaleDateString", Date::toLocaleDateString, 0},
        {"toLocaleTimeString", Date::toLocaleTimeString, 0},
        {"valueOf", Date::valueOf, 0},
    };
    for (const Entry& e : proto_methods) {
        auto fn = ObjectFactory::create_native_function(e.name, e.fn, e.arity);
        date_prototype->set_property_descriptor(e.name,
            PropertyDescriptor(Value(fn.release()), PropertyAttributes::BuiltinFunction));
    }

    Symbol* to_prim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (to_prim_sym) {
        auto to_prim_fn = ObjectFactory::create_native_function("[Symbol.toPrimitive]",
            Date::symbol_to_primitive, 1);
        date_prototype->set_property(to_prim_sym->to_property_key(), Value(to_prim_fn.release()),
            static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    }

    date_prototype->set_property_descriptor("constructor",
        PropertyDescriptor(Value(date_constructor_fn.get()),
            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable)));

    date_constructor_fn->set_property("prototype", Value(date_prototype.get()), PropertyAttributes::None);

    ctx.register_built_in_object("Date", date_constructor_fn.get());

    if (ctx.get_lexical_environment()) {
        ctx.get_lexical_environment()->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (ctx.get_variable_environment()) {
        ctx.get_variable_environment()->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (ctx.get_global_object()) {
        ctx.get_global_object()->set_property_descriptor("Date",
            PropertyDescriptor(Value(date_constructor_fn.get()), PropertyAttributes::BuiltinFunction));
    }

    date_constructor_fn.release();
    date_prototype.release();
}

} // namespace Quanta
