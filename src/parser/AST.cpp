/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/String.h"
#include <set>
#include <map>
#include <cstdio>
#include <climits>
#include <cmath>
#include <iostream>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/runtime/Math.h"
#include <cstdlib>
#include "quanta/core/runtime/String.h"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <unordered_map>
#include "ast/ast_internal.h"

namespace Quanta {

std::unordered_map<std::string, Value> g_object_function_map;

static std::unordered_map<const Context*, std::string> g_this_variable_map;

namespace {
    thread_local int loop_depth = 0;
}

int get_loop_depth() {
    return loop_depth;
}

void increment_loop_depth() {
    loop_depth++;
}

void decrement_loop_depth() {
    loop_depth--;
}


Value NumberLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value(value_);
}

std::string NumberLiteral::to_string() const {
    return std::to_string(value_);
}

std::unique_ptr<ASTNode> NumberLiteral::clone() const {
    return std::make_unique<NumberLiteral>(value_, start_, end_);
}


Value StringLiteral::evaluate(Context& ctx) {
    (void)ctx;
    // No caching: an AST-held String* is invisible to the collector and dies
    // at the first sweep; allocation is a bump-pointer hit anyway.
    return Value(new String(value_));
}

std::string StringLiteral::to_string() const {
    return "\"" + value_ + "\"";
}

std::unique_ptr<ASTNode> StringLiteral::clone() const {
    return std::make_unique<StringLiteral>(value_, start_, end_, has_escapes_);
}


Value BooleanLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value(value_);
}

std::string BooleanLiteral::to_string() const {
    return value_ ? "true" : "false";
}

std::unique_ptr<ASTNode> BooleanLiteral::clone() const {
    return std::make_unique<BooleanLiteral>(value_, start_, end_);
}


Value NullLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value::null();
}

std::string NullLiteral::to_string() const {
    return "null";
}

std::unique_ptr<ASTNode> NullLiteral::clone() const {
    return std::make_unique<NullLiteral>(start_, end_);
}


Value BigIntLiteral::evaluate(Context& ctx) {
    (void)ctx;
    try {
        auto bigint = std::make_unique<BigInt>(value_);
        return Value(bigint.release());
    } catch (const std::exception& e) {
        ctx.throw_error("Invalid BigInt literal: " + value_);
        return Value();
    }
}

std::string BigIntLiteral::to_string() const {
    return value_ + "n";
}

std::unique_ptr<ASTNode> BigIntLiteral::clone() const {
    return std::make_unique<BigIntLiteral>(value_, start_, end_);
}


Value UndefinedLiteral::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string UndefinedLiteral::to_string() const {
    return "undefined";
}

std::unique_ptr<ASTNode> UndefinedLiteral::clone() const {
    return std::make_unique<UndefinedLiteral>(start_, end_);
}


Value TemplateLiteral::evaluate(Context& ctx) {
    std::string result;
    
    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            result += element.text;
        } else if (element.type == Element::Type::EXPRESSION) {
            Value expr_value = element.expression->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            // ES6: Template literals use ToString which calls toString() on objects
            if (expr_value.is_object() || expr_value.is_function()) {
                Object* obj = expr_value.is_function() ?
                    static_cast<Object*>(expr_value.as_function()) :
                    expr_value.as_object();
                Value toString_fn = obj->get_property("toString");
                if (toString_fn.is_function()) {
                    std::vector<Value> no_args;
                    Value str_result = toString_fn.as_function()->call(ctx, no_args, expr_value);
                    if (!ctx.has_exception() && str_result.is_string()) {
                        result += str_result.to_string();
                    } else {
                        ctx.clear_exception();
                        result += expr_value.to_string();
                    }
                } else {
                    result += expr_value.to_string();
                }
            } else {
                result += expr_value.to_string();
            }
        }
    }
    
    return Value(result);
}

std::string TemplateLiteral::to_string() const {
    std::ostringstream oss;
    oss << "`";
    
    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            oss << element.text;
        } else if (element.type == Element::Type::EXPRESSION) {
            oss << "${" << element.expression->to_string() << "}";
        }
    }
    
    oss << "`";
    return oss.str();
}

std::unique_ptr<ASTNode> TemplateLiteral::clone() const {
    std::vector<Element> cloned_elements;

    for (const auto& element : elements_) {
        if (element.type == Element::Type::TEXT) {
            cloned_elements.emplace_back(element.text, element.raw_text);
        } else if (element.type == Element::Type::EXPRESSION) {
            cloned_elements.emplace_back(element.expression->clone());
        }
    }

    return std::make_unique<TemplateLiteral>(std::move(cloned_elements), start_, end_);
}


Value Parameter::evaluate(Context& ctx) {
    (void)ctx;
    return Value();
}

std::string Parameter::to_string() const {
    std::string result = "";
    if (is_rest_) {
        result += "...";
    }
    result += name_->get_name();
    if (has_default()) {
        result += " = " + default_value_->to_string();
    }
    return result;
}

std::unique_ptr<ASTNode> Parameter::clone() const {
    std::unique_ptr<ASTNode> cloned_default = default_value_ ? default_value_->clone() : nullptr;
    auto cloned = std::make_unique<Parameter>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(name_->clone().release())),
        std::move(cloned_default), is_rest_, start_, end_
    );
    if (destructuring_pattern_) {
        cloned->set_destructuring_pattern(destructuring_pattern_->clone());
    }
    return cloned;
}


Value Identifier::evaluate(Context& ctx) {
    if (name_ == "super") {
        if (ctx.this_needs_super()) {
            ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
            return Value();
        }
        Value super_constructor = ctx.get_binding("__super__");
        return super_constructor;
    }

    static const std::set<std::string> cacheable_globals = {
        "console", "Math", "JSON", "Array", "Object", "String", "Number",
        "Boolean", "RegExp", "Error", "Date", "Infinity", "NaN", "undefined"
    };

    // Globals have fast path caching (immutable bindings)
    if (cacheable_globals.find(name_) != cacheable_globals.end()) {
        if (__builtin_expect(cache_valid_, 1)) {
            return cached_value_;
        }
    }

    if (ctx.is_in_tdz(name_)) {
        ctx.throw_reference_error("Cannot access '" + name_ + "' before initialization");
        return Value();
    }

    // find_binding_env walks the scope chain exactly once (checking @@unscopables/Proxy traps at most once per env);
    // avoid has_binding + get_binding which would each re-walk the chain and double-fire traps.
    Environment* ref_env = ctx.find_binding_env(name_);
    // A Proxy `has` trap consulted while walking the scope chain may have thrown (e.g. a with-statement binding object backed by a Proxy).
    // Don't clobber that pending exception with a synthesized ReferenceError below.
    if (ctx.has_exception()) return Value();
    if (!ref_env) {
        static const std::set<std::string> known_globals = {
            "console", "Math", "JSON", "Date", "Array", "Object", "String", "Number",
            "Boolean", "RegExp", "Error", "TypeError", "ReferenceError", "SyntaxError",
            "undefined", "null", "true", "false", "Infinity", "NaN", "isNaN", "isFinite",
            "parseInt", "parseFloat", "decodeURI", "decodeURIComponent", "encodeURI",
            "encodeURIComponent", "globalThis", "window", "global", "self"
        };

        if (known_globals.find(name_) == known_globals.end()) {
            ctx.throw_reference_error("'" + name_ + "' is not defined");
            return Value();
        }
    }

    Value result = ref_env ? ref_env->get_binding_direct(name_, &ctx) : ctx.get_binding(name_);
    if (ctx.has_exception()) return Value();

    // Only cache immutable globals
    if (cacheable_globals.find(name_) != cacheable_globals.end() && !cache_valid_) {
        cached_value_ = result;
        cache_valid_ = true;
    }

    return result;
}

std::string Identifier::to_string() const {
    return name_;
}

std::unique_ptr<ASTNode> Identifier::clone() const {
    return std::make_unique<Identifier>(name_, start_, end_);
}



} // namespace Quanta
