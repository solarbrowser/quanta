/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/JsonBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/JSON.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_json_builtins(Context& ctx) {
    auto json_object = ObjectFactory::create_object();

    auto json_parse = ObjectFactory::create_native_function("parse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_parse(ctx, args);
        }, 2);
    json_object->set_property("parse", Value(json_parse.release()),
        PropertyAttributes::BuiltinFunction);

    auto json_stringify = ObjectFactory::create_native_function("stringify",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_stringify(ctx, args);
        }, 3);
    json_object->set_property("stringify", Value(json_stringify.release()),
        PropertyAttributes::BuiltinFunction);

    auto json_isRawJSON = ObjectFactory::create_native_function("isRawJSON",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();
            if (obj->has_property("rawJSON")) {
                return Value(true);
            }

            return Value(false);
        }, 1);
    json_object->set_property("isRawJSON", Value(json_isRawJSON.release()),
        PropertyAttributes::BuiltinFunction);

    PropertyDescriptor json_tag_desc(Value(std::string("JSON")), PropertyAttributes::Configurable);
    json_object->set_property_descriptor("Symbol.toStringTag", json_tag_desc);

    ctx.register_built_in_object("JSON", json_object.release());
}

} // namespace Quanta
