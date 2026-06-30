/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/NumberBuiltin.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/parser/AST.h"
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

namespace Quanta {

void register_number_builtins(Context& ctx) {
    auto number_constructor = ObjectFactory::create_native_constructor("Number",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Number() uses ToNumeric (not ToNumber): BigInt is explicitly convertible.
            double num_value = 0.0;
            if (!args.empty()) {
                if (args[0].is_bigint()) num_value = args[0].as_bigint()->to_double();
                else num_value = args[0].to_number();
            }

            // Return an ObjectType::Number-tagged wrapper so Object.prototype.toString
            // sees the correct internal-slot tag (generic Function::construct gives Ordinary).
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                auto number_obj = std::make_unique<Object>(Object::ObjectType::Number);
                number_obj->set_prototype(this_obj->get_prototype());
                number_obj->set_property("[[PrimitiveValue]]", Value(num_value), PropertyAttributes::Writable);
                return Value(number_obj.release());
            }

            // Called as a plain function: ToNumber, no wrapper
            return Value(num_value);
        });
    PropertyDescriptor max_value_desc(Value(std::numeric_limits<double>::max()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MAX_VALUE", max_value_desc);
    PropertyDescriptor min_value_desc(Value(5e-324), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MIN_VALUE", min_value_desc);
    PropertyDescriptor nan_desc(Value(std::numeric_limits<double>::quiet_NaN()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("NaN", nan_desc);
    PropertyDescriptor pos_inf_desc(Value(std::numeric_limits<double>::infinity()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("POSITIVE_INFINITY", pos_inf_desc);
    PropertyDescriptor neg_inf_desc(Value(-std::numeric_limits<double>::infinity()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("NEGATIVE_INFINITY", neg_inf_desc);
    PropertyDescriptor epsilon_desc(Value(2.220446049250313e-16), PropertyAttributes::None);
    number_constructor->set_property_descriptor("EPSILON", epsilon_desc);
    PropertyDescriptor max_safe_desc(Value(9007199254740991.0), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MAX_SAFE_INTEGER", max_safe_desc);
    PropertyDescriptor min_safe_desc(Value(-9007199254740991.0), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MIN_SAFE_INTEGER", min_safe_desc);
    
    auto isInteger_fn = ObjectFactory::create_native_function("isInteger",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            if (!args[0].is_number()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num) && std::floor(num) == num);
        }, 1);
    number_constructor->set_property("isInteger", Value(isInteger_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto numberIsNaN_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            // ES6 Number.isNaN: only returns true for actual NaN values (no type coercion)
            if (args.empty()) return Value(false);
            // Must be a number type AND NaN value
            if (!args[0].is_number()) return Value(false);
            return Value(args[0].is_nan());
        }, 1);
    number_constructor->set_property("isNaN", Value(numberIsNaN_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto numberIsFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            
            if (!args[0].is_number()) return Value(false);
            
            double val = args[0].to_number();
            
            if (val != val) return Value(false);
            
            const double MAX_FINITE = 1.7976931348623157e+308;
            return Value(val > -MAX_FINITE && val < MAX_FINITE);
        }, 1);
    number_constructor->set_property("isFinite", Value(numberIsFinite_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isSafeInteger_fn = ObjectFactory::create_native_function("isSafeInteger",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            if (!args[0].is_number()) return Value(false);
            double num = args[0].to_number();
            if (!std::isfinite(num)) return Value(false);
            if (std::floor(num) != num) return Value(false);
            const double MAX_SAFE = 9007199254740991.0;
            return Value(num >= -MAX_SAFE && num <= MAX_SAFE);
        }, 1);
    number_constructor->set_property("isSafeInteger", Value(isSafeInteger_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: Number.parseFloat/parseInt set up later after global functions are defined

    auto number_prototype = ObjectFactory::create_object();
    number_prototype->set_property("[[PrimitiveValue]]", Value(0.0));

    auto number_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_number()) {
                    return this_val;
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Number ||
                        (this_obj->has_property("[[PrimitiveValue]]") && this_obj->get_property("[[PrimitiveValue]]").is_number())) {
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_type_error("Number.prototype.valueOf requires a Number or Number wrapper");
                return Value();
            } catch (...) {
                ctx.throw_type_error("Number.prototype.valueOf requires a Number or Number wrapper");
                return Value();
            }
        }, 0);

    PropertyDescriptor number_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
    number_valueOf_name_desc.set_configurable(true);
    number_valueOf_name_desc.set_enumerable(false);
    number_valueOf_name_desc.set_writable(false);
    number_valueOf->set_property_descriptor("name", number_valueOf_name_desc);

    PropertyDescriptor number_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
    number_valueOf_length_desc.set_enumerable(false);
    number_valueOf_length_desc.set_writable(false);
    number_valueOf->set_property_descriptor("length", number_valueOf_length_desc);

    auto number_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            try {
                Value this_val = ctx.get_binding("this");
                double num = 0.0;

                if (this_val.is_number()) {
                    num = this_val.as_number();
                } else if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Number ||
                        (this_obj->has_property("[[PrimitiveValue]]") && this_obj->get_property("[[PrimitiveValue]]").is_number())) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        num = primitive.as_number();
                    } else {
                        ctx.throw_type_error("Number.prototype.toString requires a Number or Number wrapper");
                        return Value();
                    }
                } else {
                    ctx.throw_type_error("Number.prototype.toString requires a Number or Number wrapper");
                    return Value();
                }

                // Validate radix BEFORE handling NaN/Infinity (spec step 4 before step 3).
                int radix = 10;
                if (!args.empty() && !args[0].is_undefined()) {
                    if (args[0].is_symbol() || args[0].is_bigint()) {
                        ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
                        return Value();
                    }
                    double r = args[0].to_number();
                    if (ctx.has_exception()) return Value();
                    radix = std::isnan(r) ? 10 : static_cast<int>(r);
                    if (radix < 2 || radix > 36) {
                        ctx.throw_range_error("radix must be between 2 and 36");
                        return Value();
                    }
                }

                if (std::isnan(num)) return Value(std::string("NaN"));
                if (std::isinf(num)) return Value(num > 0 ? "Infinity" : "-Infinity");

                if (radix == 10) {
                    // Check if number is an integer
                    if (num == std::floor(num) && std::abs(num) < 1e15) {
                        // Format as integer
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(0) << num;
                        return Value(oss.str());
                    } else {
                        // Use default formatting for decimal numbers
                        std::ostringstream oss;
                        oss << num;
                        std::string result = oss.str();

                        // Remove trailing zeros after decimal point
                        size_t dot_pos = result.find('.');
                        if (dot_pos != std::string::npos) {
                            size_t last_nonzero = result.find_last_not_of('0');
                            if (last_nonzero > dot_pos) {
                                result = result.substr(0, last_nonzero + 1);
                            } else if (last_nonzero == dot_pos) {
                                result = result.substr(0, dot_pos);
                            }
                        }
                        return Value(result);
                    }
                }

                bool negative = num < 0;
                if (negative) num = -num;

                int64_t int_part = static_cast<int64_t>(num);
                std::string result;
                if (int_part == 0) {
                    result = "0";
                } else {
                    while (int_part > 0) {
                        int digit = int_part % radix;
                        result = (digit < 10 ? char('0' + digit) : char('a' + digit - 10)) + result;
                        int_part /= radix;
                    }
                }

                if (negative) result = "-" + result;
                return Value(result);
            } catch (...) {
                ctx.throw_type_error("Number.prototype.toString requires a Number or Number wrapper");
                return Value();
            }
        }, 1);

    PropertyDescriptor number_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    number_toString_name_desc.set_configurable(true);
    number_toString_name_desc.set_enumerable(false);
    number_toString_name_desc.set_writable(false);
    number_toString->set_property_descriptor("name", number_toString_name_desc);

    PropertyDescriptor number_toString_length_desc(Value(1.0), PropertyAttributes::Configurable);
    number_toString_length_desc.set_enumerable(false);
    number_toString_length_desc.set_writable(false);
    number_toString->set_property_descriptor("length", number_toString_length_desc);

    PropertyDescriptor number_valueOf_desc(Value(number_valueOf.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("valueOf", number_valueOf_desc);
    PropertyDescriptor number_toString_desc(Value(number_toString.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toString", number_toString_desc);

    auto toExponential_fn = ObjectFactory::create_native_function("toExponential",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num;
            if (this_val.is_number()) {
                num = this_val.as_number();
            } else if (this_val.is_object()) {
                Value pv = this_val.as_object()->get_property("[[PrimitiveValue]]");
                if (!pv.is_number()) { ctx.throw_type_error("Number method requires a Number or Number wrapper"); return Value(); }
                num = pv.as_number();
            } else { ctx.throw_type_error("Number method requires a Number or Number wrapper"); return Value(); }

            // Spec order: 1) ToIntegerOrInfinity(frac) -- can throw, 2) check NaN/Inf, 3) check range.
            bool has_frac = !args.empty() && !args[0].is_undefined();
            int frac = 0;
            double frac_int = 0.0;
            if (has_frac) {
                if (args[0].is_symbol() || args[0].is_bigint()) { ctx.throw_type_error("Cannot convert Symbol/BigInt to number"); return Value(); }
                double frac_d = args[0].to_number();
                if (ctx.has_exception()) return Value();
                frac_int = std::isnan(frac_d) ? 0.0 : std::trunc(frac_d);
                frac = static_cast<int>(frac_int);
            }

            // NaN/Infinity check AFTER ToInteger but BEFORE range check.
            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            if (has_frac && (frac_int < 0 || frac_int > 100)) {
                ctx.throw_range_error("toExponential() precision out of range");
                return Value();
            }

            bool negative = num < 0;

            // Use snprintf directly for correct IEEE 754 representation.
            char buf[200];
            if (!has_frac) {
                // Undefined fractionDigits: use minimum representation (%.17e then strip trailing zeros).
                snprintf(buf, sizeof(buf), "%.17e", std::abs(num));
            } else {
                snprintf(buf, sizeof(buf), "%.*e", frac, std::abs(num));
            }
            std::string s(buf);
            size_t e_pos = s.find('e');
            if (e_pos == std::string::npos) return Value(s);

            std::string m_part = s.substr(0, e_pos);
            if (!has_frac) {
                // Strip trailing zeros from mantissa.
                if (m_part.find('.') != std::string::npos) {
                    size_t last = m_part.find_last_not_of('0');
                    if (last != std::string::npos && m_part[last] == '.') m_part = m_part.substr(0, last);
                    else if (last != std::string::npos) m_part = m_part.substr(0, last + 1);
                }
            }
            int parsed_exp = std::stoi(s.substr(e_pos + 1));
            std::string result;
            if (negative) result += "-";
            result += m_part;
            result += "e";
            result += (parsed_exp >= 0) ? "+" : "-";
            result += std::to_string(std::abs(parsed_exp));
            return Value(result);
        }, 1);
    PropertyDescriptor toExponential_desc(Value(toExponential_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toExponential", toExponential_desc);

    auto toFixed_fn = ObjectFactory::create_native_function("toFixed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();

            int precision = 0;
            if (!args.empty() && !args[0].is_undefined()) {
                if (args[0].is_symbol() || args[0].is_bigint()) {
                    ctx.throw_type_error("Cannot convert Symbol/BigInt to number");
                    return Value();
                }
                double frac = args[0].to_number();
                if (ctx.has_exception()) return Value();
                // ToIntegerOrInfinity: NaN->0, then check [0,100]
                double int_frac = std::isnan(frac) ? 0.0 : std::trunc(frac);
                if (int_frac < 0 || int_frac > 100) {
                    ctx.throw_range_error("toFixed() precision out of range");
                    return Value();
                }
                precision = static_cast<int>(int_frac);
            }

            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            bool negative = num < 0;
            double abs_num = negative ? -num : num;
            double factor = std::pow(10.0, precision);
            abs_num = std::floor(abs_num * factor + 0.5) / factor;

            char buffer[256];
            std::string format = "%." + std::to_string(precision) + "f";
            snprintf(buffer, sizeof(buffer), format.c_str(), abs_num);

            std::string result;
            if (negative && abs_num != 0) result += "-";
            result += buffer;
            return Value(result);
        }, 1);
    PropertyDescriptor toFixed_desc(Value(toFixed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toFixed", toFixed_desc);

    auto toPrecision_fn = ObjectFactory::create_native_function("toPrecision",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num;
            if (this_val.is_number()) { num = this_val.as_number(); }
            else if (this_val.is_object()) {
                Value pv = this_val.as_object()->get_property("[[PrimitiveValue]]");
                if (!pv.is_number()) { ctx.throw_type_error("Number method requires a Number or Number wrapper"); return Value(); }
                num = pv.as_number();
            } else { ctx.throw_type_error("Number method requires a Number or Number wrapper"); return Value(); }

            if (args.empty() || args[0].is_undefined()) {
                if (!std::isfinite(num)) return Value(std::isnan(num) ? std::string("NaN") : (num > 0 ? std::string("Infinity") : std::string("-Infinity")));
                return Value(Value(num).to_string());
            }

            if (args[0].is_symbol() || args[0].is_bigint()) { ctx.throw_type_error("Cannot convert Symbol/BigInt to number"); return Value(); }
            double prec_d = args[0].to_number();
            if (ctx.has_exception()) return Value();
            double prec_int = std::isnan(prec_d) ? 0.0 : std::trunc(prec_d);
            // Spec step 4: handle NaN/Infinity BEFORE range check (step 5).
            if (!std::isfinite(num)) return Value(std::isnan(num) ? std::string("NaN") : (num > 0 ? std::string("Infinity") : std::string("-Infinity")));
            int precision = static_cast<int>(prec_int);
            if (prec_int < 1 || prec_int > 100) {
                ctx.throw_range_error("toPrecision() precision out of range");
                return Value();
            }

            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            bool negative = num < 0;
            double abs_num = negative ? -num : num;

            int exp = 0;
            if (abs_num != 0) {
                exp = static_cast<int>(std::floor(std::log10(abs_num)));
                double test_m = abs_num / std::pow(10.0, exp);
                if (test_m >= 10.0) exp++;
                else if (test_m < 1.0) exp--;
            }

            char buf[256];
            if (exp >= 0 && exp < precision) {
                int frac_digits = precision - exp - 1;
                double factor = std::pow(10.0, frac_digits);
                double rounded = std::floor(abs_num * factor + 0.5) / factor;
                snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), rounded);
                std::string result;
                if (negative) result += "-";
                result += buf;
                return Value(result);
            } else if (exp < 0 && exp >= -6) {
                int frac_digits = precision - exp - 1;
                double factor = std::pow(10.0, frac_digits);
                double rounded = std::floor(abs_num * factor + 0.5) / factor;
                snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), rounded);
                std::string result;
                if (negative) result += "-";
                result += buf;
                return Value(result);
            } else {
                int frac_digits = precision - 1;
                double mantissa = (abs_num == 0) ? 0.0 : abs_num / std::pow(10.0, exp);
                if (mantissa >= 10.0) { mantissa /= 10.0; exp++; }
                else if (mantissa < 1.0 && mantissa > 0) { mantissa *= 10.0; exp--; }
                double factor = std::pow(10.0, frac_digits);
                mantissa = std::floor(mantissa * factor + 0.5) / factor;
                if (mantissa >= 10.0) { mantissa /= 10.0; exp++; }

                if (frac_digits > 0)
                    snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), mantissa);
                else
                    snprintf(buf, sizeof(buf), "%.0f", mantissa);

                std::string result;
                if (negative) result += "-";
                result += buf;
                result += "e";
                result += (exp >= 0) ? "+" : "-";
                result += std::to_string(std::abs(exp));
                return Value(result);
            }
        }, 1);
    PropertyDescriptor toPrecision_desc(Value(toPrecision_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toPrecision", toPrecision_desc);

    auto number_toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_val = ctx.get_binding("this");
            return Value(this_val.to_string());
        });
    PropertyDescriptor number_toLocaleString_desc(Value(number_toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toLocaleString", number_toLocaleString_desc);

    PropertyDescriptor number_constructor_desc(Value(number_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("constructor", number_constructor_desc);

    auto isNaN_fn2 = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            // ES6 Number.isNaN: only returns true for actual NaN values (no type coercion)
            if (args.empty()) return Value(false);
            // Must be a number type AND NaN value
            if (!args[0].is_number()) return Value(false);
            return Value(args[0].is_nan());
        }, 1);
    number_constructor->set_property("isNaN", Value(isNaN_fn2.release()), PropertyAttributes::BuiltinFunction);

    auto isFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_number()) return Value(false);
            return Value(std::isfinite(args[0].to_number()));
        }, 1);
    number_constructor->set_property("isFinite", Value(isFinite_fn.release()), PropertyAttributes::BuiltinFunction);
    number_constructor->set_property("prototype", Value(number_prototype.release()), PropertyAttributes::None);

    ctx.register_built_in_object("Number", number_constructor.release());
}

} // namespace Quanta
