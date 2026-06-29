/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/SymbolBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_symbol_builtins(Context& ctx) {
    auto symbol_constructor = ObjectFactory::create_native_constructor("Symbol",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.is_in_constructor_call()) {
                ctx.throw_type_error("Symbol is not a constructor");
                return Value();
            }
            bool has_desc = !args.empty() && !args[0].is_undefined();
            std::string description = has_desc ? args[0].to_string() : "";
            if (ctx.has_exception()) return Value();
            auto symbol = Symbol::create(description, has_desc);
            return Value(symbol.release());
        }, 0);
    
    auto symbol_for_fn = ObjectFactory::create_native_function("for",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_for(ctx, args);
        }, 1);
    symbol_constructor->set_property("for", Value(symbol_for_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto symbol_key_for_fn = ObjectFactory::create_native_function("keyFor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_key_for(ctx, args);
        }, 1);
    symbol_constructor->set_property("keyFor", Value(symbol_key_for_fn.release()), PropertyAttributes::BuiltinFunction);

    Symbol* iterator_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_sym) {
        symbol_constructor->set_property_descriptor("iterator", PropertyDescriptor(Value(iterator_sym), PropertyAttributes::None));
    }
    
    Symbol* async_iterator_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_sym) {
        symbol_constructor->set_property_descriptor("asyncIterator", PropertyDescriptor(Value(async_iterator_sym), PropertyAttributes::None));
    }
    
    Symbol* match_sym = Symbol::get_well_known(Symbol::MATCH);
    if (match_sym) {
        symbol_constructor->set_property_descriptor("match", PropertyDescriptor(Value(match_sym), PropertyAttributes::None));
    }

    Symbol* match_all_sym = Symbol::get_well_known(Symbol::MATCH_ALL);
    if (match_all_sym) {
        symbol_constructor->set_property_descriptor("matchAll", PropertyDescriptor(Value(match_all_sym), PropertyAttributes::None));
    }

    Symbol* replace_sym = Symbol::get_well_known(Symbol::REPLACE);
    if (replace_sym) {
        symbol_constructor->set_property_descriptor("replace", PropertyDescriptor(Value(replace_sym), PropertyAttributes::None));
    }
    
    Symbol* search_sym = Symbol::get_well_known(Symbol::SEARCH);
    if (search_sym) {
        symbol_constructor->set_property_descriptor("search", PropertyDescriptor(Value(search_sym), PropertyAttributes::None));
    }
    
    Symbol* split_sym = Symbol::get_well_known(Symbol::SPLIT);
    if (split_sym) {
        symbol_constructor->set_property_descriptor("split", PropertyDescriptor(Value(split_sym), PropertyAttributes::None));
    }
    
    Symbol* has_instance_sym = Symbol::get_well_known(Symbol::HAS_INSTANCE);
    if (has_instance_sym) {
        symbol_constructor->set_property_descriptor("hasInstance", PropertyDescriptor(Value(has_instance_sym), PropertyAttributes::None));
    }
    
    Symbol* is_concat_spreadable_sym = Symbol::get_well_known(Symbol::IS_CONCAT_SPREADABLE);
    if (is_concat_spreadable_sym) {
        symbol_constructor->set_property_descriptor("isConcatSpreadable", PropertyDescriptor(Value(is_concat_spreadable_sym), PropertyAttributes::None));
    }
    
    Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
    if (species_sym) {
        symbol_constructor->set_property_descriptor("species", PropertyDescriptor(Value(species_sym), PropertyAttributes::None));
    }
    
    Symbol* to_primitive_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (to_primitive_sym) {
        symbol_constructor->set_property_descriptor("toPrimitive", PropertyDescriptor(Value(to_primitive_sym), PropertyAttributes::None));
    }
    
    Symbol* to_string_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (to_string_tag_sym) {
        symbol_constructor->set_property_descriptor("toStringTag", PropertyDescriptor(Value(to_string_tag_sym), PropertyAttributes::None));
    }
    
    Symbol* unscopables_sym = Symbol::get_well_known(Symbol::UNSCOPABLES);
    if (unscopables_sym) {
        symbol_constructor->set_property_descriptor("unscopables", PropertyDescriptor(Value(unscopables_sym), PropertyAttributes::None));
    }

    Symbol* dispose_sym = Symbol::get_well_known(Symbol::DISPOSE);
    if (dispose_sym) {
        symbol_constructor->set_property_descriptor("dispose", PropertyDescriptor(Value(dispose_sym), PropertyAttributes::None));
    }

    Symbol* async_dispose_sym = Symbol::get_well_known(Symbol::ASYNC_DISPOSE);
    if (async_dispose_sym) {
        symbol_constructor->set_property_descriptor("asyncDispose", PropertyDescriptor(Value(async_dispose_sym), PropertyAttributes::None));
    }

    {
        auto sym_proto = ObjectFactory::create_object();
        sym_proto->set_property("constructor", Value(symbol_constructor.get()), PropertyAttributes::BuiltinFunction);
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("Symbol")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sym_proto->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
        auto desc_getter = ObjectFactory::create_native_function("get description",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                // Primitive symbol this
                Value prim = ctx.get_binding("__primitive_this__");
                if (prim.is_symbol()) return prim.as_symbol()->get_has_description() ? Value(prim.as_symbol()->get_description()) : Value();
                // Symbol wrapper object this (e.g. Object(sym))
                Object* obj = ctx.get_this_binding();
                if (obj) {
                    Value inner = obj->get_property("[[PrimitiveValue]]");
                    if (inner.is_symbol()) return inner.as_symbol()->get_has_description() ? Value(inner.as_symbol()->get_description()) : Value();
                }
                ctx.throw_type_error("Symbol.prototype.description requires a symbol");
                return Value();
            });
        PropertyDescriptor desc_prop;
        desc_prop.set_getter(desc_getter.release());
        desc_prop.set_enumerable(false);
        desc_prop.set_configurable(true);
        sym_proto->set_property_descriptor("description", desc_prop);

        auto valueOf_fn = ObjectFactory::create_native_function("valueOf",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Value prim = ctx.get_binding("__primitive_this__");
                if (prim.is_symbol()) return prim;
                Object* obj = ctx.get_this_binding();
                if (obj) {
                    Value inner = obj->get_property("[[PrimitiveValue]]");
                    if (inner.is_symbol()) return inner;
                }
                ctx.throw_type_error("Symbol.prototype.valueOf requires a symbol");
                return Value();
            });
        sym_proto->set_property("valueOf", Value(valueOf_fn.release()), PropertyAttributes::BuiltinFunction);

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Value prim = ctx.get_binding("__primitive_this__");
                if (prim.is_symbol()) return Value(prim.as_symbol()->to_string());
                Object* obj = ctx.get_this_binding();
                if (obj) {
                    Value inner = obj->get_property("[[PrimitiveValue]]");
                    if (inner.is_symbol()) return Value(inner.as_symbol()->to_string());
                }
                ctx.throw_type_error("Symbol.prototype.toString requires a symbol");
                return Value();
            });
        sym_proto->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

        // Symbol.prototype[Symbol.toPrimitive] -- returns the raw symbol value, which causes
        // TypeError when callers try to ToSTRING it (e.g. "".indexOf(Object(Symbol()))).
        Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
        if (toPrim_sym) {
            auto sym_toPrimitive = ObjectFactory::create_native_function("[Symbol.toPrimitive]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Value prim = ctx.get_binding("__primitive_this__");
                    if (prim.is_symbol()) return prim;
                    Object* obj = ctx.get_this_binding();
                    if (obj) {
                        Value inner = obj->get_property("[[PrimitiveValue]]");
                        if (inner.is_symbol()) return inner;
                    }
                    ctx.throw_type_error("Symbol.prototype[Symbol.toPrimitive] requires a symbol");
                    return Value();
                }, 1);
            sym_proto->set_property(toPrim_sym->to_property_key(), Value(sym_toPrimitive.release()), PropertyAttributes::Configurable);
        }

        symbol_constructor->set_property("prototype", Value(sym_proto.release()), PropertyAttributes::None);
    }

    ctx.register_built_in_object("Symbol", symbol_constructor.release());
}

} // namespace Quanta
