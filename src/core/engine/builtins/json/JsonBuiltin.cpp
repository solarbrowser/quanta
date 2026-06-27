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

    // JSON.rawJSON(text): creates a frozen null-prototype object with "rawJSON" property
    auto json_rawJSON = ObjectFactory::create_native_function("rawJSON",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_syntax_error("JSON.rawJSON: text must be a valid JSON primitive");
                return Value();
            }
            if (args[0].is_symbol()) {
                ctx.throw_type_error("JSON.rawJSON: argument cannot be converted to string");
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_syntax_error("JSON.rawJSON: text is not valid JSON");
                return Value();
            }
            std::string json_string = args[0].to_string();
            if (ctx.has_exception()) return Value();

            // Validate: outermost must not be object/array
            if (!json_string.empty() && (json_string[0] == '{' || json_string[0] == '[')) {
                ctx.throw_syntax_error("JSON.rawJSON: value must not be an object or array");
                return Value();
            }
            // Try to parse as valid JSON (catch invalid text)
            try {
                JSON::parse(json_string);
            } catch (...) {
                ctx.throw_syntax_error("JSON.rawJSON: text is not valid JSON");
                return Value();
            }
            // Validate: empty or leading/trailing whitespace -> SyntaxError
            if (json_string.empty()) {
                ctx.throw_syntax_error("JSON.rawJSON: text must not be empty");
                return Value();
            }
            char first = json_string.front(), last = json_string.back();
            if (first == '\t' || first == '\n' || first == '\r' || first == ' ' ||
                last  == '\t' || last  == '\n' || last  == '\r' || last  == ' ') {
                ctx.throw_syntax_error("JSON.rawJSON: text must not start or end with whitespace");
                return Value();
            }

            auto obj = ObjectFactory::create_object();
            obj->set_prototype(nullptr);
            PropertyDescriptor raw_desc(Value(json_string),
                static_cast<PropertyAttributes>(PropertyAttributes::Enumerable | PropertyAttributes::Configurable | PropertyAttributes::Writable));
            obj->set_property_descriptor("rawJSON", raw_desc);

            return Value(obj.release());
        }, 1);
    json_object->set_property("rawJSON", Value(json_rawJSON.release()),
        PropertyAttributes::BuiltinFunction);

    auto json_isRawJSON = ObjectFactory::create_native_function("isRawJSON",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) return Value(false);
            Object* obj = args[0].as_object();
            // rawJSON objects: null prototype + own "rawJSON" string property
            if (obj->get_prototype() != nullptr) return Value(false);
            Value raw = obj->get_property("rawJSON");
            return Value(!raw.is_undefined());
        }, 1);
    json_object->set_property("isRawJSON", Value(json_isRawJSON.release()),
        PropertyAttributes::BuiltinFunction);

    PropertyDescriptor json_tag_desc(Value(std::string("JSON")), PropertyAttributes::Configurable);
    json_object->set_property_descriptor("Symbol.toStringTag", json_tag_desc);

    ctx.register_built_in_object("JSON", json_object.release());
}

} // namespace Quanta
