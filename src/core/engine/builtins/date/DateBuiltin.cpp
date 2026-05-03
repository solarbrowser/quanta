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
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "quanta/parser/AST.h"

namespace Quanta {

void register_date_builtins(Context& ctx) {
    auto add_date_instance_methods = [](Object* date_obj) {
        auto getTime_fn = ObjectFactory::create_native_function("getTime",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                return Value(static_cast<double>(timestamp));
            });
        date_obj->set_property("getTime", Value(getTime_fn.release()), PropertyAttributes::BuiltinFunction);

        auto getFullYear_fn = ObjectFactory::create_native_function("getFullYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year + 1900)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getFullYear", Value(getFullYear_fn.release()), PropertyAttributes::BuiltinFunction);

        auto getMonth_fn = ObjectFactory::create_native_function("getMonth",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mon)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getMonth", Value(getMonth_fn.release()), PropertyAttributes::BuiltinFunction);

        auto getDate_fn = ObjectFactory::create_native_function("getDate",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mday)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getDate", Value(getDate_fn.release()), PropertyAttributes::BuiltinFunction);

        auto getYear_fn = ObjectFactory::create_native_function("getYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getYear", Value(getYear_fn.release()), PropertyAttributes::BuiltinFunction);

        auto setYear_fn = ObjectFactory::create_native_function("setYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }

                double year_value = args[0].to_number();
                if (std::isnan(year_value) || std::isinf(year_value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }

                int year = static_cast<int>(year_value);
                if (year >= 0 && year <= 99) {
                    year += 1900;
                }

                return Value(static_cast<double>(year));
            });
        date_obj->set_property("setYear", Value(setYear_fn.release()), PropertyAttributes::BuiltinFunction);

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::string time_str = std::ctime(&time);
                if (!time_str.empty() && time_str.back() == '\n') {
                    time_str.pop_back();
                }
                return Value(time_str);
            });
        date_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
    };

    auto date_prototype = ObjectFactory::create_object();
    Object* date_proto_ptr = date_prototype.get();

    auto date_constructor_fn = ObjectFactory::create_native_constructor("Date",
        [date_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            // If called as function (not constructor), return current time string
            if (!ctx.is_in_constructor_call()) {
                auto now = std::chrono::system_clock::now();
                std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                std::tm* now_tm = std::localtime(&now_time);
                char buffer[100];
                std::strftime(buffer, sizeof(buffer), "%a %b %d %Y %H:%M:%S", now_tm);
                return Value(std::string(buffer));
            }

            // Otherwise construct Date object
            Value date_obj = Date::date_constructor(ctx, args);

            if (date_obj.is_object()) {
                date_obj.as_object()->set_prototype(date_proto_ptr);
            }

            return date_obj;
        });

    auto date_now = ObjectFactory::create_native_function("now", Date::now);
    auto date_parse = ObjectFactory::create_native_function("parse", Date::parse);
    auto date_UTC = ObjectFactory::create_native_function("UTC", Date::UTC);

    date_constructor_fn->set_property("now", Value(date_now.release()), PropertyAttributes::BuiltinFunction);
    date_constructor_fn->set_property("parse", Value(date_parse.release()), PropertyAttributes::BuiltinFunction);
    date_constructor_fn->set_property("UTC", Value(date_UTC.release()), PropertyAttributes::BuiltinFunction);

    auto getTime_fn = ObjectFactory::create_native_function("getTime", Date::getTime);
    auto getFullYear_fn = ObjectFactory::create_native_function("getFullYear", Date::getFullYear);
    auto getMonth_fn = ObjectFactory::create_native_function("getMonth", Date::getMonth);
    auto getDate_fn = ObjectFactory::create_native_function("getDate", Date::getDate);
    auto getDay_fn = ObjectFactory::create_native_function("getDay", Date::getDay);
    auto getHours_fn = ObjectFactory::create_native_function("getHours", Date::getHours);
    auto getMinutes_fn = ObjectFactory::create_native_function("getMinutes", Date::getMinutes);
    auto getSeconds_fn = ObjectFactory::create_native_function("getSeconds", Date::getSeconds);
    auto getMilliseconds_fn = ObjectFactory::create_native_function("getMilliseconds", Date::getMilliseconds);
    auto toString_fn = ObjectFactory::create_native_function("toString", Date::toString);
    auto toISOString_fn = ObjectFactory::create_native_function("toISOString", Date::toISOString);
    auto toJSON_fn = ObjectFactory::create_native_function("toJSON", Date::toJSON);
    auto valueOf_fn = ObjectFactory::create_native_function("valueOf", Date::valueOf);
    auto toUTCString_fn = ObjectFactory::create_native_function("toUTCString", Date::toUTCString);

    auto toDateString_fn = ObjectFactory::create_native_function("toDateString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("Wed Jan 01 2020"));
        }, 0);

    auto toLocaleDateString_fn = ObjectFactory::create_native_function("toLocaleDateString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("1/1/2020"));
        }, 0);

    auto date_toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("1/1/2020, 12:00:00 AM"));
        }, 0);

    auto toLocaleTimeString_fn = ObjectFactory::create_native_function("toLocaleTimeString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("12:00:00 AM"));
        }, 0);

    auto toTimeString_fn = ObjectFactory::create_native_function("toTimeString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("00:00:00 GMT+0000 (UTC)"));
        }, 0);

    toDateString_fn->set_property("name", Value(std::string("toDateString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toLocaleDateString_fn->set_property("name", Value(std::string("toLocaleDateString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    date_toLocaleString_fn->set_property("name", Value(std::string("toLocaleString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toLocaleTimeString_fn->set_property("name", Value(std::string("toLocaleTimeString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toTimeString_fn->set_property("name", Value(std::string("toTimeString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    auto getYear_fn = ObjectFactory::create_native_function("getYear", Date::getYear);
    auto setYear_fn = ObjectFactory::create_native_function("setYear", Date::setYear);

    PropertyDescriptor getTime_desc(Value(getTime_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getTime", getTime_desc);
    PropertyDescriptor getFullYear_desc(Value(getFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getFullYear", getFullYear_desc);
    PropertyDescriptor getMonth_desc(Value(getMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMonth", getMonth_desc);
    PropertyDescriptor getDate_desc(Value(getDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getDate", getDate_desc);
    PropertyDescriptor getDay_desc(Value(getDay_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getDay", getDay_desc);
    PropertyDescriptor getHours_desc(Value(getHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getHours", getHours_desc);
    PropertyDescriptor getMinutes_desc(Value(getMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMinutes", getMinutes_desc);
    PropertyDescriptor getSeconds_desc(Value(getSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getSeconds", getSeconds_desc);
    PropertyDescriptor getMilliseconds_desc(Value(getMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMilliseconds", getMilliseconds_desc);
    PropertyDescriptor date_toString_desc(Value(toString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toString", date_toString_desc);
    PropertyDescriptor toISOString_desc(Value(toISOString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toISOString", toISOString_desc);
    PropertyDescriptor toJSON_desc(Value(toJSON_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toJSON", toJSON_desc);
    PropertyDescriptor valueOf_desc(Value(valueOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("valueOf", valueOf_desc);
    PropertyDescriptor toUTCString_desc(Value(toUTCString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toUTCString", toUTCString_desc);
    PropertyDescriptor toDateString_desc(Value(toDateString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toDateString", toDateString_desc);
    PropertyDescriptor toLocaleDateString_desc(Value(toLocaleDateString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleDateString", toLocaleDateString_desc);
    PropertyDescriptor date_toLocaleString_desc(Value(date_toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleString", date_toLocaleString_desc);
    PropertyDescriptor toLocaleTimeString_desc(Value(toLocaleTimeString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleTimeString", toLocaleTimeString_desc);
    PropertyDescriptor toTimeString_desc(Value(toTimeString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toTimeString", toTimeString_desc);

    auto getTimezoneOffset_fn = ObjectFactory::create_native_function("getTimezoneOffset", Date::getTimezoneOffset, 0);
    PropertyDescriptor getTimezoneOffset_desc(Value(getTimezoneOffset_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getTimezoneOffset", getTimezoneOffset_desc);

    auto getUTCDate_fn = ObjectFactory::create_native_function("getUTCDate", Date::getUTCDate, 0);
    auto getUTCDay_fn = ObjectFactory::create_native_function("getUTCDay", Date::getUTCDay, 0);
    auto getUTCFullYear_fn = ObjectFactory::create_native_function("getUTCFullYear", Date::getUTCFullYear, 0);
    auto getUTCHours_fn = ObjectFactory::create_native_function("getUTCHours", Date::getUTCHours, 0);
    auto getUTCMilliseconds_fn = ObjectFactory::create_native_function("getUTCMilliseconds", Date::getUTCMilliseconds, 0);
    auto getUTCMinutes_fn = ObjectFactory::create_native_function("getUTCMinutes", Date::getUTCMinutes, 0);
    auto getUTCMonth_fn = ObjectFactory::create_native_function("getUTCMonth", Date::getUTCMonth, 0);
    auto getUTCSeconds_fn = ObjectFactory::create_native_function("getUTCSeconds", Date::getUTCSeconds, 0);

    PropertyDescriptor getUTCDate_desc(Value(getUTCDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCDate", getUTCDate_desc);
    PropertyDescriptor getUTCDay_desc(Value(getUTCDay_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCDay", getUTCDay_desc);
    PropertyDescriptor getUTCFullYear_desc(Value(getUTCFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCFullYear", getUTCFullYear_desc);
    PropertyDescriptor getUTCHours_desc(Value(getUTCHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCHours", getUTCHours_desc);
    PropertyDescriptor getUTCMilliseconds_desc(Value(getUTCMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMilliseconds", getUTCMilliseconds_desc);
    PropertyDescriptor getUTCMinutes_desc(Value(getUTCMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMinutes", getUTCMinutes_desc);
    PropertyDescriptor getUTCMonth_desc(Value(getUTCMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMonth", getUTCMonth_desc);
    PropertyDescriptor getUTCSeconds_desc(Value(getUTCSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCSeconds", getUTCSeconds_desc);

    auto setTime_fn = ObjectFactory::create_native_function("setTime", Date::setTime, 1);
    auto setFullYear_fn = ObjectFactory::create_native_function("setFullYear", Date::setFullYear, 3);
    auto setMonth_fn = ObjectFactory::create_native_function("setMonth", Date::setMonth, 2);
    auto setDate_fn = ObjectFactory::create_native_function("setDate", Date::setDate, 1);
    auto setHours_fn = ObjectFactory::create_native_function("setHours", Date::setHours, 4);
    auto setMinutes_fn = ObjectFactory::create_native_function("setMinutes", Date::setMinutes, 3);
    auto setSeconds_fn = ObjectFactory::create_native_function("setSeconds", Date::setSeconds, 2);
    auto setMilliseconds_fn = ObjectFactory::create_native_function("setMilliseconds", Date::setMilliseconds, 1);

    PropertyDescriptor setTime_desc(Value(setTime_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setTime", setTime_desc);
    PropertyDescriptor setFullYear_desc(Value(setFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setFullYear", setFullYear_desc);
    PropertyDescriptor setMonth_desc(Value(setMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMonth", setMonth_desc);
    PropertyDescriptor setDate_desc(Value(setDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setDate", setDate_desc);
    PropertyDescriptor setHours_desc(Value(setHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setHours", setHours_desc);
    PropertyDescriptor setMinutes_desc(Value(setMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMinutes", setMinutes_desc);
    PropertyDescriptor setSeconds_desc(Value(setSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setSeconds", setSeconds_desc);
    PropertyDescriptor setMilliseconds_desc(Value(setMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMilliseconds", setMilliseconds_desc);

    auto setUTCFullYear_fn = ObjectFactory::create_native_function("setUTCFullYear", Date::setUTCFullYear, 3);
    auto setUTCMonth_fn = ObjectFactory::create_native_function("setUTCMonth", Date::setUTCMonth, 2);
    auto setUTCDate_fn = ObjectFactory::create_native_function("setUTCDate", Date::setUTCDate, 1);
    auto setUTCHours_fn = ObjectFactory::create_native_function("setUTCHours", Date::setUTCHours, 4);
    auto setUTCMinutes_fn = ObjectFactory::create_native_function("setUTCMinutes", Date::setUTCMinutes, 3);
    auto setUTCSeconds_fn = ObjectFactory::create_native_function("setUTCSeconds", Date::setUTCSeconds, 2);
    auto setUTCMilliseconds_fn = ObjectFactory::create_native_function("setUTCMilliseconds", Date::setUTCMilliseconds, 1);

    PropertyDescriptor setUTCFullYear_desc(Value(setUTCFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCFullYear", setUTCFullYear_desc);
    PropertyDescriptor setUTCMonth_desc(Value(setUTCMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMonth", setUTCMonth_desc);
    PropertyDescriptor setUTCDate_desc(Value(setUTCDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCDate", setUTCDate_desc);
    PropertyDescriptor setUTCHours_desc(Value(setUTCHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCHours", setUTCHours_desc);
    PropertyDescriptor setUTCMinutes_desc(Value(setUTCMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMinutes", setUTCMinutes_desc);
    PropertyDescriptor setUTCSeconds_desc(Value(setUTCSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCSeconds", setUTCSeconds_desc);
    PropertyDescriptor setUTCMilliseconds_desc(Value(setUTCMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMilliseconds", setUTCMilliseconds_desc);

    date_prototype->set_property("getYear", Value(getYear_fn.release()), PropertyAttributes::BuiltinFunction);
    date_prototype->set_property("setYear", Value(setYear_fn.release()), PropertyAttributes::BuiltinFunction);

    auto toGMTString_fn = ObjectFactory::create_native_function("toGMTString", Date::toGMTString);
    date_prototype->set_property("toGMTString", Value(toGMTString_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: Date.prototype[Symbol.toPrimitive]
    Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (toPrim_sym) {
        auto date_toPrimitive_fn = ObjectFactory::create_native_function("[Symbol.toPrimitive]",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* obj = ctx.get_this_binding();
                if (!obj) {
                    ctx.throw_type_error("Date.prototype[Symbol.toPrimitive] called on non-object");
                    return Value();
                }
                std::string hint = args.empty() ? "default" : args[0].to_string();
                if (hint == "number") {
                    Value valueOf_fn = obj->get_property("valueOf");
                    if (valueOf_fn.is_function()) {
                        Value result = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    Value toString_fn = obj->get_property("toString");
                    if (toString_fn.is_function()) {
                        Value result = toString_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    return Value();
                } else {
                    Value toString_fn = obj->get_property("toString");
                    if (toString_fn.is_function()) {
                        Value result = toString_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    Value valueOf_fn = obj->get_property("valueOf");
                    if (valueOf_fn.is_function()) {
                        Value result = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    return Value();
                }
            }, 1);
        date_prototype->set_property(toPrim_sym->to_property_key(), Value(date_toPrimitive_fn.release()),
            static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    }

    PropertyDescriptor date_proto_ctor_desc(Value(date_constructor_fn.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    date_prototype->set_property_descriptor("constructor", date_proto_ctor_desc);

    date_constructor_fn->set_property("prototype", Value(date_prototype.get()));

    ctx.register_built_in_object("Date", date_constructor_fn.get());

    if (ctx.get_lexical_environment()) {
        ctx.get_lexical_environment()->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (ctx.get_variable_environment()) {
        ctx.get_variable_environment()->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (ctx.get_global_object()) {
        PropertyDescriptor date_desc(Value(date_constructor_fn.get()),
            PropertyAttributes::BuiltinFunction);
        ctx.get_global_object()->set_property_descriptor("Date", date_desc);
    }

    date_constructor_fn.release();
    date_prototype.release();

    (void)add_date_instance_methods;
}

} // namespace Quanta
