/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/IntlBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"

namespace Quanta {

void register_intl_builtins(Context& ctx) {
    auto intl_object = ObjectFactory::create_object();

    auto intl_datetimeformat = ObjectFactory::create_native_constructor("DateTimeFormat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto formatter = ObjectFactory::create_object();

            auto format_fn = ObjectFactory::create_native_function("format",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string("Invalid Date"));
                    }
                    return Value(std::string("1/1/1970"));
                }, 1);
            formatter->set_property("format", Value(format_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(formatter.release());
        });
    intl_object->set_property("DateTimeFormat", Value(intl_datetimeformat.release()));

    auto intl_numberformat = ObjectFactory::create_native_constructor("NumberFormat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto formatter = ObjectFactory::create_object();

            auto format_fn = ObjectFactory::create_native_function("format",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string("0"));
                    }
                    return Value(args[0].to_string());
                }, 1);
            formatter->set_property("format", Value(format_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(formatter.release());
        });
    intl_object->set_property("NumberFormat", Value(intl_numberformat.release()));

    auto intl_collator = ObjectFactory::create_native_constructor("Collator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto collator = ObjectFactory::create_object();

            auto compare_fn = ObjectFactory::create_native_function("compare",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() < 2) return Value(0.0);
                    std::string a = args[0].to_string();
                    std::string b = args[1].to_string();
                    if (a < b) return Value(-1.0);
                    if (a > b) return Value(1.0);
                    return Value(0.0);
                }, 2);
            collator->set_property("compare", Value(compare_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(collator.release());
        });
    intl_object->set_property("Collator", Value(intl_collator.release()));

    ctx.register_built_in_object("Intl", intl_object.release());
}

} // namespace Quanta
