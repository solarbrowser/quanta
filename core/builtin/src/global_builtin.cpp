/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "global_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../include/Engine.h"
#include "../../parser/include/AST.h"
#include <vector>
#include <memory>
#include <cmath>
#include <cctype>
#include <sstream>

namespace Quanta {

void GlobalBuiltin::register_global_functions(Context& ctx) {
    register_parse_functions(ctx);
    register_type_check_functions(ctx);
    register_encoding_functions(ctx);
    register_eval_function(ctx);
}

void GlobalBuiltin::register_parse_functions(Context& ctx) {
    // parseInt(string, radix)
    auto parseInt_fn = ObjectFactory::create_native_function("parseInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            std::string str = args[0].to_string();
            int radix = 10;

            if (args.size() > 1) {
                double radix_val = args[1].to_number();
                if (std::isnan(radix_val) || radix_val == 0) {
                    radix = 10;
                } else {
                    radix = static_cast<int>(radix_val);
                    if (radix < 2 || radix > 36) {
                        return Value(std::numeric_limits<double>::quiet_NaN());
                    }
                }
            }

            // Skip leading whitespace
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }

            if (start >= str.length()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            // Handle sign
            bool negative = false;
            if (str[start] == '+') {
                start++;
            } else if (str[start] == '-') {
                negative = true;
                start++;
            }

            // Auto-detect hexadecimal if radix is 16 or unspecified
            if ((radix == 16 || radix == 10) && start + 1 < str.length() &&
                str[start] == '0' && (str[start + 1] == 'x' || str[start + 1] == 'X')) {
                radix = 16;
                start += 2;
            }

            double result = 0.0;
            bool found_digit = false;

            for (size_t i = start; i < str.length(); i++) {
                char c = str[i];
                int digit;

                if (c >= '0' && c <= '9') {
                    digit = c - '0';
                } else if (c >= 'a' && c <= 'z') {
                    digit = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'Z') {
                    digit = c - 'A' + 10;
                } else {
                    break; // Stop at first non-digit character
                }

                if (digit >= radix) {
                    break; // Stop if digit is invalid for this radix
                }

                result = result * radix + digit;
                found_digit = true;
            }

            if (!found_digit) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(negative ? -result : result);
        });
    ctx.get_global_object()->set_property("parseInt", Value(parseInt_fn.release()));

    // parseFloat(string)
    auto parseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            std::string str = args[0].to_string();

            // Skip leading whitespace
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }

            if (start >= str.length()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            // Try to parse as number
            char* endptr;
            double result = std::strtod(str.c_str() + start, &endptr);

            // If no characters were parsed, return NaN
            if (endptr == str.c_str() + start) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(result);
        });
    ctx.get_global_object()->set_property("parseFloat", Value(parseFloat_fn.release()));
}

void GlobalBuiltin::register_type_check_functions(Context& ctx) {
    // isNaN(value)
    auto isNaN_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(true);
            }
            double val = args[0].to_number();
            return Value(std::isnan(val));
        });
    ctx.get_global_object()->set_property("isNaN", Value(isNaN_fn.release()));

    // isFinite(value)
    auto isFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(false);
            }
            double val = args[0].to_number();
            return Value(std::isfinite(val));
        });
    ctx.get_global_object()->set_property("isFinite", Value(isFinite_fn.release()));
}

void GlobalBuiltin::register_encoding_functions(Context& ctx) {
    // escape(string)
    auto escape_fn = ObjectFactory::create_native_function("escape",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value("undefined");
            }

            std::string str = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < str.length(); i++) {
                char c = str[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '*' || c == '+' ||
                    c == '-' || c == '.' || c == '/' || c == '@' || c == '_') {
                    result += c;
                } else {
                    std::ostringstream oss;
                    oss << '%' << std::hex << std::uppercase << (unsigned char)c;
                    result += oss.str();
                }
            }

            return Value(result);
        });
    ctx.get_global_object()->set_property("escape", Value(escape_fn.release()));

    // unescape(string)
    auto unescape_fn = ObjectFactory::create_native_function("unescape",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value("undefined");
            }

            std::string str = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < str.length(); i++) {
                if (str[i] == '%' && i + 2 < str.length()) {
                    // Try to parse hex escape sequence
                    char hex1 = str[i + 1];
                    char hex2 = str[i + 2];

                    if (std::isxdigit(hex1) && std::isxdigit(hex2)) {
                        int value = 0;
                        if (hex1 >= '0' && hex1 <= '9') value += (hex1 - '0') * 16;
                        else if (hex1 >= 'A' && hex1 <= 'F') value += (hex1 - 'A' + 10) * 16;
                        else if (hex1 >= 'a' && hex1 <= 'f') value += (hex1 - 'a' + 10) * 16;

                        if (hex2 >= '0' && hex2 <= '9') value += (hex2 - '0');
                        else if (hex2 >= 'A' && hex2 <= 'F') value += (hex2 - 'A' + 10);
                        else if (hex2 >= 'a' && hex2 <= 'f') value += (hex2 - 'a' + 10);

                        result += static_cast<char>(value);
                        i += 2; // Skip the hex digits
                    } else {
                        result += str[i];
                    }
                } else {
                    result += str[i];
                }
            }

            return Value(result);
        });
    ctx.get_global_object()->set_property("unescape", Value(unescape_fn.release()));
}

void GlobalBuiltin::register_eval_function(Context& ctx) {
    // eval(code)
    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value();
            }

            // Only eval strings
            if (!args[0].is_string()) {
                return args[0]; // Return non-string values unchanged
            }

            std::string code = args[0].to_string();

            try {
                // Simple eval implementation - just return the string for now
                // TODO: Implement proper eval functionality
                return Value(code);
            } catch (...) {
                ctx.throw_exception(Value("SyntaxError: Invalid or unexpected token"));
                return Value();
            }
        });
    ctx.get_global_object()->set_property("eval", Value(eval_fn.release()));
}

} // namespace Quanta