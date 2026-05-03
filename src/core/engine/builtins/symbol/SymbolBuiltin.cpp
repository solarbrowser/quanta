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
            bool has_desc = !args.empty() && !args[0].is_undefined();
            std::string description = has_desc ? args[0].to_string() : "";
            auto symbol = Symbol::create(description, has_desc);
            return Value(symbol.release());
        });
    
    auto symbol_for_fn = ObjectFactory::create_native_function("for",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_for(ctx, args);
        });
    symbol_constructor->set_property("for", Value(symbol_for_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto symbol_key_for_fn = ObjectFactory::create_native_function("keyFor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_key_for(ctx, args);
        });
    symbol_constructor->set_property("keyFor", Value(symbol_key_for_fn.release()), PropertyAttributes::BuiltinFunction);

    Symbol* iterator_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_sym) {
        symbol_constructor->set_property("iterator", Value(iterator_sym));
    }
    
    Symbol* async_iterator_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_sym) {
        symbol_constructor->set_property("asyncIterator", Value(async_iterator_sym));
    }
    
    Symbol* match_sym = Symbol::get_well_known(Symbol::MATCH);
    if (match_sym) {
        symbol_constructor->set_property("match", Value(match_sym));
    }
    
    Symbol* replace_sym = Symbol::get_well_known(Symbol::REPLACE);
    if (replace_sym) {
        symbol_constructor->set_property("replace", Value(replace_sym));
    }
    
    Symbol* search_sym = Symbol::get_well_known(Symbol::SEARCH);
    if (search_sym) {
        symbol_constructor->set_property("search", Value(search_sym));
    }
    
    Symbol* split_sym = Symbol::get_well_known(Symbol::SPLIT);
    if (split_sym) {
        symbol_constructor->set_property("split", Value(split_sym));
    }
    
    Symbol* has_instance_sym = Symbol::get_well_known(Symbol::HAS_INSTANCE);
    if (has_instance_sym) {
        symbol_constructor->set_property("hasInstance", Value(has_instance_sym));
    }
    
    Symbol* is_concat_spreadable_sym = Symbol::get_well_known(Symbol::IS_CONCAT_SPREADABLE);
    if (is_concat_spreadable_sym) {
        symbol_constructor->set_property("isConcatSpreadable", Value(is_concat_spreadable_sym));
    }
    
    Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
    if (species_sym) {
        symbol_constructor->set_property("species", Value(species_sym));
    }
    
    Symbol* to_primitive_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (to_primitive_sym) {
        symbol_constructor->set_property("toPrimitive", Value(to_primitive_sym));
    }
    
    Symbol* to_string_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (to_string_tag_sym) {
        symbol_constructor->set_property("toStringTag", Value(to_string_tag_sym));
    }
    
    Symbol* unscopables_sym = Symbol::get_well_known(Symbol::UNSCOPABLES);
    if (unscopables_sym) {
        symbol_constructor->set_property("unscopables", Value(unscopables_sym));
    }
    
    {
        auto sym_proto = ObjectFactory::create_object();
        sym_proto->set_property("constructor", Value(symbol_constructor.get()));
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("Symbol")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sym_proto->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
        auto desc_getter = ObjectFactory::create_native_function("get description",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Value prim = ctx.get_binding("__primitive_this__");
                if (prim.is_symbol()) return prim.as_symbol()->get_has_description() ? Value(prim.as_symbol()->get_description()) : Value();
                return Value();
            });
        PropertyDescriptor desc_prop;
        desc_prop.set_getter(desc_getter.release());
        desc_prop.set_enumerable(false);
        desc_prop.set_configurable(true);
        sym_proto->set_property_descriptor("description", desc_prop);
        symbol_constructor->set_property("prototype", Value(sym_proto.release()));
    }

    ctx.register_built_in_object("Symbol", symbol_constructor.release());
}

} // namespace Quanta
