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
#include "quanta/core/gc/Collector.h"
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




std::string NumberLiteral::to_string() const {
    return Value(value_).to_string();
}

std::unique_ptr<ASTNode> NumberLiteral::clone() const {
    return std::make_unique<NumberLiteral>(value_, start_, end_);
}




std::string StringLiteral::to_string() const {
    return "\"" + value_ + "\"";
}

std::unique_ptr<ASTNode> StringLiteral::clone() const {
    return std::make_unique<StringLiteral>(value_, start_, end_, has_escapes_);
}




std::string BooleanLiteral::to_string() const {
    return value_ ? "true" : "false";
}

std::unique_ptr<ASTNode> BooleanLiteral::clone() const {
    return std::make_unique<BooleanLiteral>(value_, start_, end_);
}




std::string NullLiteral::to_string() const {
    return "null";
}

std::unique_ptr<ASTNode> NullLiteral::clone() const {
    return std::make_unique<NullLiteral>(start_, end_);
}




std::string BigIntLiteral::to_string() const {
    return value_ + "n";
}

std::unique_ptr<ASTNode> BigIntLiteral::clone() const {
    return std::make_unique<BigIntLiteral>(value_, start_, end_);
}




std::string UndefinedLiteral::to_string() const {
    return "undefined";
}

std::unique_ptr<ASTNode> UndefinedLiteral::clone() const {
    return std::make_unique<UndefinedLiteral>(start_, end_);
}


std::string TemplateLiteral::stringify_element(Context& ctx, const Value& v) {
    // A substitution is ToString(value). This used to call the object's
    // toString directly, which skipped @@toPrimitive entirely, never refused a
    // symbol, and cleared whatever the call threw before falling back to a
    // default rendering -- so a throwing toString produced a string instead of
    // propagating.
    if (v.is_symbol()) {
        ctx.throw_type_error("Cannot convert a Symbol value to a string");
        return std::string();
    }
    if (v.is_object() || v.is_function()) {
        Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
        Value prim = obj->to_primitive("string");
        if (ctx.has_exception()) return std::string();
        if (prim.is_symbol()) {
            ctx.throw_type_error("Cannot convert a Symbol value to a string");
            return std::string();
        }
        return prim.to_string();
    }
    return v.to_string();
}

namespace {

// One process-wide store, registered with the collector once and never
// released. Sites index into it; a freed site returns its slot for reuse so an
// eval loop cannot grow this without bound.
std::vector<Value>& template_object_store() {
    static std::vector<Value>* store = [] {
        auto* v = new std::vector<Value>();
        Collector::push_value_vector(v);
        return v;
    }();
    return *store;
}

std::vector<int32_t>& template_object_free_slots() {
    static std::vector<int32_t> slots;
    return slots;
}

}  // namespace

TemplateLiteral::~TemplateLiteral() {
    if (template_object_slot_ < 0) return;
    template_object_store()[template_object_slot_] = Value();
    template_object_free_slots().push_back(template_object_slot_);
}

Value TemplateLiteral::cached_template_object() const {
    if (template_object_slot_ < 0) return Value();
    return template_object_store()[template_object_slot_];
}

void TemplateLiteral::cache_template_object(const Value& obj) {
    auto& store = template_object_store();
    if (template_object_slot_ < 0) {
        auto& free_slots = template_object_free_slots();
        if (!free_slots.empty()) {
            template_object_slot_ = free_slots.back();
            free_slots.pop_back();
        } else {
            template_object_slot_ = static_cast<int32_t>(store.size());
            store.push_back(Value());
        }
    }
    store[template_object_slot_] = obj;
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




std::string Identifier::to_string() const {
    return get_name();
}

std::unique_ptr<ASTNode> Identifier::clone() const {
    return std::make_unique<Identifier>(get_name(), start_, end_);
}

const char* EngineHelper::slot_name(Kind kind) {
    switch (kind) {
        case Kind::DefineField:         return "__deffield__";
        case Kind::PrivateFieldAdd:     return "__pfadd__";
        case Kind::SetFunctionName:     return "__setfnname__";
        case Kind::ClassFieldInitEnter: return "__cfi_enter__";
        case Kind::ClassFieldInitExit:  return "__cfi_exit__";
        case Kind::ImportSource:        return "__import_source__";
        case Kind::ImportDefer:         return "__import_defer__";
        case Kind::ClassFieldKey:       return "__classkey__";
    }
    return "";
}



std::string EngineHelper::to_string() const {
    return slot_name(kind_);
}

std::unique_ptr<ASTNode> EngineHelper::clone() const {
    return std::make_unique<EngineHelper>(kind_, start_, end_);
}



} // namespace Quanta
