/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/JSON.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <algorithm>

namespace Quanta {


Value JSON::parse(const std::string& json_string, const ParseOptions& options) {
    Parser parser(json_string, options);
    return parser.parse();
}

Value JSON::parse(const std::string& json_string, const ParseOptions& options, SourceMap& out_source_map,
        std::string& out_root_source) {
    Parser parser(json_string, options);
    Value result = parser.parse();
    out_source_map = parser.source_map();
    out_root_source = parser.root_source();
    return result;
}

std::string JSON::stringify(const Value& value, const StringifyOptions& options) {
    Stringifier stringifier(options, nullptr);
    return stringifier.stringify(value);
}

// ES6 24.3.1.1 InternalizeJSONProperty, extended with the json-parse-with-source "context" arg.
static Value internalize_json_property(Context& ctx, Object* holder, const std::string& name, Function* reviver,
        const JSON::SourceMap& source_map) {
    Value val = holder->get_property(name);

    if (val.is_object_like() && val.as_object()) {
        Object* obj = val.as_object();
        // IsArray: unwrap Proxies recursively like Array.isArray does.
        Object* unwrapped = obj;
        while (unwrapped && unwrapped->get_type() == Object::ObjectType::Proxy) {
            Proxy* p = static_cast<Proxy*>(unwrapped);
            if (p->is_revoked()) { ctx.throw_type_error("IsArray called on revoked Proxy"); return Value(); }
            unwrapped = p->get_proxy_target();
        }
        if (unwrapped && unwrapped->is_array()) {
            // Array: walk by integer index
            Value len_val = obj->get_property("length");
            uint32_t len = static_cast<uint32_t>(len_val.to_number());
            for (uint32_t i = 0; i < len; i++) {
                std::string idx = std::to_string(i);
                Value new_element = internalize_json_property(ctx, obj, idx, reviver, source_map);
                if (ctx.has_exception()) return Value();
                if (new_element.is_undefined()) {
                    obj->delete_property(idx);
                    if (ctx.has_exception()) return Value();
                } else {
                    // CreateDataProperty: {value:V, writable:true, enumerable:true, configurable:true}
                    PropertyDescriptor d(new_element, static_cast<PropertyAttributes>(
                        PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
                    obj->set_property_descriptor(idx, d);
                    if (ctx.has_exception()) return Value();
                }
            }
        } else {
            // Object: walk enumerable own string keys (via proxy ownKeys trap if applicable).
            std::vector<std::string> keys;
            if (obj->get_type() == Object::ObjectType::Proxy) {
                try {
                    keys = static_cast<Proxy*>(obj)->own_keys_trap();
                    if (ctx.has_exception()) return Value();
                } catch (const std::runtime_error&) {
                    if (!ctx.has_exception()) ctx.throw_type_error("'ownKeys' proxy invariant violated");
                    return Value();
                }
            } else {
                keys = obj->get_own_property_keys();
            }
            for (const auto& key : keys) {
                Value new_element = internalize_json_property(ctx, obj, key, reviver, source_map);
                if (ctx.has_exception()) return Value();
                if (new_element.is_undefined()) {
                    obj->delete_property(key);
                    if (ctx.has_exception()) return Value();
                } else {
                    PropertyDescriptor d(new_element, static_cast<PropertyAttributes>(
                        PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
                    obj->set_property_descriptor(key, d);
                    if (ctx.has_exception()) return Value();
                }
            }
        }
    }

    // context = { source } only if `val` is still the exact primitive originally parsed here.
    auto context = ObjectFactory::create_object();
    auto parent_it = source_map.find(holder);
    if (parent_it != source_map.end()) {
        auto key_it = parent_it->second.find(name);
        if (key_it != parent_it->second.end() && key_it->second.second.strict_equals(val)) {
            context->set_property("source", Value(key_it->second.first),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        }
    }

    // Call reviver(name, val, context)
    std::vector<Value> reviver_args;
    reviver_args.push_back(Value(name));
    reviver_args.push_back(val);
    reviver_args.push_back(Value(context.release()));
    return reviver->call(ctx, reviver_args, Value(holder));
}

Value JSON::js_parse(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_syntax_error("JSON.parse requires at least 1 argument");
        return Value();
    }

    // Symbol cannot be converted to string (TypeError per spec)
    if (args[0].is_symbol()) {
        ctx.throw_type_error("JSON.parse: cannot convert Symbol to string");
        return Value();
    }

    // For objects, call JS ToPrimitive (hint "string"): try toString, then valueOf.
    std::string json_string;
    if ((args[0].is_object() || args[0].is_function()) && args[0].as_object()) {
        Value ts_fn = args[0].as_object()->get_property("toString");
        if (ctx.has_exception()) return Value();
        if (ts_fn.is_function()) {
            Value ts_result = ts_fn.as_function()->call(ctx, {}, args[0]);
            if (ctx.has_exception()) return Value();
            json_string = ts_result.to_string();
        } else {
            Value vs_fn = args[0].as_object()->get_property("valueOf");
            if (ctx.has_exception()) return Value();
            if (vs_fn.is_function()) {
                Value vs_result = vs_fn.as_function()->call(ctx, {}, args[0]);
                if (ctx.has_exception()) return Value();
                json_string = vs_result.to_string();
            } else {
                json_string = args[0].to_string();
            }
        }
    } else {
        json_string = args[0].to_string();
    }

    if (json_string.empty()) {
        ctx.throw_syntax_error("JSON.parse: unexpected end of input");
        return Value();
    }

    ParseOptions options;

    Function* reviver = nullptr;
    // Process reviver parameter (args[1])
    if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
        if (args[1].is_function()) {
            reviver = args[1].as_function();
        }
    }

    try {
        SourceMap source_map;
        std::string root_source;
        Value result = reviver ? parse(json_string, options, source_map, root_source) : parse(json_string, options);

        // Apply reviver via InternalizeJSONProperty walk
        if (reviver) {
            // Wrapper must have Object.prototype and use CreateDataPropertyOrThrow (not [[Set]])
            auto wrapper = ObjectFactory::create_object();
            PropertyDescriptor root_desc(result, static_cast<PropertyAttributes>(
                PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
            wrapper->set_property_descriptor("", root_desc);
            if (!root_source.empty()) {
                source_map[wrapper.get()][""] = {root_source, result};
            }

            result = internalize_json_property(ctx, wrapper.get(), "", reviver, source_map);
            wrapper.release();

            if (ctx.has_exception()) {
                return Value();
            }
        }

        return result;
    } catch (const std::exception& e) {
        ctx.throw_syntax_error("JSON.parse error: " + std::string(e.what()));
        return Value();
    } catch (...) {
        ctx.throw_syntax_error("JSON.parse: unknown parsing error");
        return Value();
    }
}

Value JSON::js_stringify(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value();
    }

    if (args[0].is_undefined() || args[0].is_symbol() || args[0].is_function()) {
        return Value();  // undefined - functions/symbols/undefined are not serializable at top level
    }

    if (args[0].is_object() && !args[0].as_object()) {
        return Value(std::string("null"));
    }

    StringifyOptions options;

    // Process replacer parameter (args[1])
    if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
        if (args[1].is_function()) {
            options.replacer_function = args[1].as_function();
        } else if (args[1].is_object()) {
            Object* replacer_obj = args[1].as_object();

            Object* effective_replacer = replacer_obj;
            if (replacer_obj && replacer_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* p = static_cast<Proxy*>(replacer_obj);
                if (p->is_revoked()) {
                    ctx.throw_type_error("Cannot perform 'IsArray' on a revoked proxy");
                    return Value();
                }
                effective_replacer = p->get_proxy_target();
            }
            if (replacer_obj && effective_replacer && effective_replacer->is_array()) {
                options.has_replacer_array = true;
                Value length_val = replacer_obj->get_property("length");
                uint32_t length = static_cast<uint32_t>(length_val.to_number());
                std::vector<std::string>& ra = options.replacer_array;
                for (uint32_t i = 0; i < length; i++) {
                    Value item = replacer_obj->get_element(i);
                    std::string s;
                    if (item.is_string()) {
                        s = item.to_string();
                    } else if (item.is_number()) {
                        s = item.to_string();
                    // Spec: only String/Number wrapper objects (with [[StringData]]/[[NumberData]])
                    } else if (item.is_object() && item.as_object()) {
                        Object* w = item.as_object();
                        auto t = w->get_type();
                        if (t == Object::ObjectType::String || t == Object::ObjectType::Number) {
                            // Call toString() on the wrapper (spec: ToString(item))
                            Value ts_fn = w->get_property("toString");
                            if (ts_fn.is_function()) {
                                Value r = ts_fn.as_function()->call(ctx, {}, item);
                                if (!ctx.has_exception()) s = r.to_string();
                                else ctx.clear_exception();
                            }
                        }
                    }
                    if (!s.empty() && std::find(ra.begin(), ra.end(), s) == ra.end())
                        ra.push_back(s);
                }
            }
        }
    }

    if (args.size() > 2 && !args[2].is_undefined()) {
        Value space_arg = args[2];
        // Unwrap Number/String wrapper objects (call valueOf/toString per spec)
        if (space_arg.is_object() && space_arg.as_object() && space_arg.as_object()->is_primitive_wrapper()) {
            Object* sw = space_arg.as_object();
            auto st = sw->get_type();
            if (st == Object::ObjectType::Number) {
                Value vof = sw->get_property("valueOf");
                if (vof.is_function()) {
                    Value r = vof.as_function()->call(ctx, {}, space_arg);
                    if (ctx.has_exception()) return Value();
                    space_arg = r;
                }
            } else if (st == Object::ObjectType::String) {
                Value tsf = sw->get_property("toString");
                if (tsf.is_function()) {
                    Value r = tsf.as_function()->call(ctx, {}, space_arg);
                    if (ctx.has_exception()) return Value();
                    space_arg = r;
                }
            }
        }
        if (space_arg.is_number()) {
            int indent_count = static_cast<int>(space_arg.to_number());
            if (indent_count > 0) {
                options.indent = std::string(std::min(indent_count, 10), ' ');
            }
        } else if (space_arg.is_string()) {
            options.indent = space_arg.to_string().substr(0, 10);
        }
    }

    try {
        Stringifier stringifier(options, &ctx);
        std::string result = stringifier.stringify(args[0]);
        if (ctx.has_exception()) return Value();
        if (result.empty() || result == "undefined_sentinel") return Value();
        return Value(result);
    } catch (const std::exception& e) {
        ctx.throw_type_error(std::string(e.what()));
        return Value();
    }
}

std::unique_ptr<Object> JSON::create_json_object() {
    auto json_obj = std::make_unique<Object>();
    
    json_obj->set_property("parse", Value(std::string("function JSON.parse() { [native code] }")));
    json_obj->set_property("stringify", Value(std::string("function JSON.stringify() { [native code] }")));
    
    return json_obj;
}


JSON::Parser::Parser(const std::string& json, const ParseOptions& options) 
    : json_(json), position_(0), line_(1), column_(1), depth_(0), options_(options) {
}

Value JSON::Parser::parse() {
    skip_whitespace();

    if (at_end()) {
        throw_syntax_error("Unexpected end of JSON input");
    }

    size_t start = position_;
    Value result = parse_value();
    if (result.is_number() || result.is_boolean() || result.is_string() || result.is_null()) {
        root_source_ = json_.substr(start, position_ - start);
    }

    skip_whitespace();
    if (!at_end()) {
        throw_syntax_error("Unexpected token after JSON value");
    }

    return result;
}

void JSON::Parser::record_source(Object* parent, const std::string& key, size_t start, const Value& value) {
    if (!(value.is_number() || value.is_boolean() || value.is_string() || value.is_null())) return;
    source_map_[parent][key] = {json_.substr(start, position_ - start), value};
}

Value JSON::Parser::parse_value() {
    skip_whitespace();
    
    if (at_end()) {
        throw_syntax_error("Unexpected end of JSON input");
    }
    
    char ch = current_char();
    
    switch (ch) {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
            return parse_string();
        case 't':
        case 'f':
            return parse_boolean();
        case 'n':
            return parse_null();
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number();
        default:
            throw_syntax_error("Unexpected token: " + std::string(1, ch));
    }
}

Value JSON::Parser::parse_object() {
    if (++depth_ > options_.max_depth) {
        throw_syntax_error("nesting depth exceeded");
    }
    
    advance();
    skip_whitespace();
    
    // Use create_object so parsed objects inherit from Object.prototype.
    auto obj = ObjectFactory::create_object();

    if (current_char() == '}') {
        advance();
        depth_--;
        return Value(obj.release());
    }

    while (true) {
        skip_whitespace();

        if (current_char() != '"') {
            throw_syntax_error("Expected string key in object");
        }

        std::string key = parse_string_literal();

        skip_whitespace();
        if (current_char() != ':') {
            throw_syntax_error("Expected ':' after object key");
        }
        advance();

        skip_whitespace();
        size_t value_start = position_;
        Value value = parse_value();
        record_source(obj.get(), key, value_start, value);

        PropertyDescriptor d(value, static_cast<PropertyAttributes>(
            PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        obj->set_property_descriptor(key, d);
        
        skip_whitespace();
        char ch = current_char();
        
        if (ch == '}') {
            advance();
            break;
        } else if (ch == ',') {
            advance();
            skip_whitespace();
            
            if (current_char() == '}') {
                if (options_.allow_trailing_commas) {
                    advance();
                    break;
                } else {
                    throw_syntax_error("Trailing comma not allowed");
                }
            }
        } else {
            throw_syntax_error("Expected ',' or '}' in object");
        }
    }
    
    depth_--;
    return Value(obj.release());
}

Value JSON::Parser::parse_array() {
    if (++depth_ > options_.max_depth) {
        throw_syntax_error("nesting depth exceeded");
    }
    
    advance();
    skip_whitespace();
    
    auto arr = ObjectFactory::create_array(0);
    uint32_t index = 0;
    
    if (current_char() == ']') {
        advance();
        arr->set_property("length", Value(0.0));
        depth_--;
        return Value(arr.release());
    }
    
    while (true) {
        skip_whitespace();
        size_t value_start = position_;
        Value value = parse_value();
        record_source(arr.get(), std::to_string(index), value_start, value);
        arr->set_element(index++, value);

        skip_whitespace();
        char ch = current_char();
        
        if (ch == ']') {
            advance();
            break;
        } else if (ch == ',') {
            advance();
            skip_whitespace();
            
            if (current_char() == ']') {
                if (options_.allow_trailing_commas) {
                    advance();
                    break;
                } else {
                    throw_syntax_error("Trailing comma not allowed");
                }
            }
        } else {
            throw_syntax_error("Expected ',' or ']' in array");
        }
    }
    
    arr->set_property("length", Value(static_cast<double>(index)));
    depth_--;
    return Value(arr.release());
}

Value JSON::Parser::parse_string() {
    std::string str = parse_string_literal();
    return Value(str);
}

std::string JSON::Parser::parse_string_literal() {
    advance();
    std::string result;
    
    while (!at_end() && current_char() != '"') {
        char ch = current_char();
        
        if (ch == '\\') {
            advance();
            if (at_end()) {
                throw_syntax_error("Unterminated string");
            }
            
            result += parse_escape_sequence();
        } else if (ch < 0x20) {
            throw_syntax_error("Unescaped control character in string");
        } else {
            result += ch;
            advance();
        }
    }
    
    if (at_end()) {
        throw_syntax_error("Unterminated string");
    }
    
    advance();
    return result;
}

std::string JSON::Parser::parse_escape_sequence() {
    char ch = current_char();
    advance();
    
    switch (ch) {
        case '"':  return "\"";
        case '\\': return "\\";
        case '/':  return "/";
        case 'b':  return "\b";
        case 'f':  return "\f";
        case 'n':  return "\n";
        case 'r':  return "\r";
        case 't':  return "\t";
        case 'u': {
            uint32_t codepoint = parse_unicode_escape();
            if (codepoint < 128) {
                return std::string(1, static_cast<char>(codepoint));
            } else {
                return "?";
            }
        }
        default:
            throw_syntax_error("Invalid escape sequence: \\" + std::string(1, ch));
    }
}

uint32_t JSON::Parser::parse_unicode_escape() {
    uint32_t codepoint = 0;
    
    for (int i = 0; i < 4; i++) {
        if (at_end() || !is_hex_digit(current_char())) {
            throw_syntax_error("Invalid unicode escape sequence");
        }
        
        char ch = current_char();
        advance();
        
        codepoint *= 16;
        if (ch >= '0' && ch <= '9') {
            codepoint += ch - '0';
        } else if (ch >= 'A' && ch <= 'F') {
            codepoint += ch - 'A' + 10;
        } else if (ch >= 'a' && ch <= 'f') {
            codepoint += ch - 'a' + 10;
        }
    }
    
    return codepoint;
}

Value JSON::Parser::parse_number() {
    double result = parse_number_literal();
    return Value(result);
}

double JSON::Parser::parse_number_literal() {
    std::string number_str;
    
    if (current_char() == '-') {
        number_str += current_char();
        advance();
    }
    
    if (current_char() == '0') {
        number_str += current_char();
        advance();
    } else if (is_digit(current_char())) {
        while (!at_end() && is_digit(current_char())) {
            number_str += current_char();
            advance();
        }
    } else {
        throw_syntax_error("Invalid number");
    }
    
    if (!at_end() && current_char() == '.') {
        number_str += current_char();
        advance();
        
        if (at_end() || !is_digit(current_char())) {
            throw_syntax_error("Invalid number");
        }
        
        while (!at_end() && is_digit(current_char())) {
            number_str += current_char();
            advance();
        }
    }
    
    if (!at_end() && (current_char() == 'e' || current_char() == 'E')) {
        number_str += current_char();
        advance();
        
        if (!at_end() && (current_char() == '+' || current_char() == '-')) {
            number_str += current_char();
            advance();
        }
        
        if (at_end() || !is_digit(current_char())) {
            throw_syntax_error("Invalid number");
        }
        
        while (!at_end() && is_digit(current_char())) {
            number_str += current_char();
            advance();
        }
    }
    
    try {
        return std::stod(number_str);
    } catch (const std::out_of_range&) {
        return number_str[0] == '-' ? -std::numeric_limits<double>::infinity()
                                    : std::numeric_limits<double>::infinity();
    }
}

Value JSON::Parser::parse_boolean() {
    if (json_.substr(position_, 4) == "true") {
        position_ += 4;
        column_ += 4;
        return Value(true);
    } else if (json_.substr(position_, 5) == "false") {
        position_ += 5;
        column_ += 5;
        return Value(false);
    } else {
        throw_syntax_error("Invalid boolean value");
    }
}

Value JSON::Parser::parse_null() {
    if (json_.substr(position_, 4) == "null") {
        position_ += 4;
        column_ += 4;
        return Value::null();
    } else {
        throw_syntax_error("Invalid null value");
    }
}

char JSON::Parser::current_char() const {
    if (at_end()) return '\0';
    return json_[position_];
}

char JSON::Parser::peek_char(size_t offset) const {
    if (position_ + offset >= json_.length()) return '\0';
    return json_[position_ + offset];
}

void JSON::Parser::advance() {
    if (at_end()) return;
    
    if (json_[position_] == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    
    position_++;
}

void JSON::Parser::skip_whitespace() {
    while (!at_end() && is_whitespace(current_char())) {
        advance();
    }
}

bool JSON::Parser::at_end() const {
    return position_ >= json_.length();
}

void JSON::Parser::throw_syntax_error(const std::string& message) {
    std::ostringstream oss;
    oss << "JSON parse error at line " << line_ << ", column " << column_ << ": " << message;
    throw std::runtime_error(oss.str());
}

bool JSON::Parser::is_digit(char ch) const {
    return ch >= '0' && ch <= '9';
}

bool JSON::Parser::is_hex_digit(char ch) const {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
}


JSON::Stringifier::Stringifier(const StringifyOptions& options, Context* ctx)
    : options_(options), depth_(0), context_(ctx), current_key_("") {
}

// GetV(v, "toJSON") semantics: returns the toJSON function preserving primitive receiver for accessors.
Function* JSON::Stringifier::get_to_json(const Value& v) {
    if (!context_) return nullptr;
    if ((v.is_object() || v.is_function()) && v.as_object()) {
        Value tj = const_cast<Object*>(v.as_object())->get_property("toJSON");
        if (tj.is_function()) return tj.as_function();
        return nullptr;
    }
    if (v.is_bigint()) {
        Value bigint_ctor = context_->get_binding("BigInt");
        if (bigint_ctor.is_function()) {
            Value bp = bigint_ctor.as_function()->get_property("prototype");
            if (bp.is_object()) {
                PropertyDescriptor d = bp.as_object()->get_property_descriptor("toJSON");
                if (d.is_data_descriptor() && d.get_value().is_function()) return d.get_value().as_function();
                if (d.is_accessor_descriptor() && d.get_getter()) {
                    Function* getter = dynamic_cast<Function*>(d.get_getter());
                    if (getter) {
                        Value tj = getter->call(*context_, {}, v);
                        if (!context_->has_exception() && tj.is_function()) return tj.as_function();
                    }
                }
            }
        }
    }
    return nullptr;
}

std::string JSON::Stringifier::stringify(const Value& value) {
    if (options_.replacer_function && context_) {
        // Apply toJSON on root value BEFORE passing to replacer (spec step 2, also for BigInt).
        Value root = value;
        Function* to_json = get_to_json(root);
        if (to_json) {
            root = to_json->call(*context_, {Value(std::string(""))}, root);
            if (context_->has_exception()) return "";
        }

        std::vector<Value> args = { Value(std::string("")), root };

        auto wrapper = ObjectFactory::create_object();
        PropertyDescriptor wrap_desc(value, static_cast<PropertyAttributes>(
            PropertyAttributes::Writable | PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
        wrapper->set_property_descriptor("", wrap_desc);

        Value result = options_.replacer_function->call(*context_, args, Value(wrapper.get()));
        wrapper.release();

        if (context_->has_exception()) return "";
        if (result.is_undefined() || result.is_function() || result.is_symbol()) return "";
        current_key_ = "";
        skip_bigint_toJSON_ = true;
        std::string out = stringify_value(result);
        skip_bigint_toJSON_ = false;
        return out;
    }

    current_key_ = "";
    // For BigInt primitives without a replacer, apply toJSON before serialising.
    if (value.is_bigint() && context_) {
        Function* to_json_noRepl = get_to_json(value);
        if (to_json_noRepl) {
            Value after_toJSON = to_json_noRepl->call(*context_, {Value(std::string(""))}, value);
            if (context_->has_exception()) return "";
            skip_bigint_toJSON_ = true;
            std::string out = stringify_value(after_toJSON);
            skip_bigint_toJSON_ = false;
            return out;
        }
    }
    return stringify_value(value);
}

std::string JSON::Stringifier::stringify_value(const Value& value) {
    if (value.is_null()) {
        return "null";
    } else if (value.is_undefined()) {
        return "null"; // arrays: undefined elements -> null; objects skip before calling here
    } else if (value.is_bigint()) {
        if (context_ && !skip_bigint_toJSON_) {
            Function* to_json = get_to_json(value);
            if (to_json) {
                std::vector<Value> tj_args = { Value(current_key_) };
                Value result = to_json->call(*context_, tj_args, value);
                if (context_->has_exception()) return "";
                if (!result.is_bigint()) return stringify_value(result);
            }
        }
        if (context_) context_->throw_type_error("Do not know how to serialize a BigInt");
        return "";
    } else if (value.is_symbol()) {
        return "null";
    } else if (value.is_function()) {
        return "null";
    } else if (value.is_boolean()) {
        return stringify_boolean(value.to_boolean());
    } else if (value.is_number()) {
        if (value.is_nan() || value.is_positive_infinity() || value.is_negative_infinity()) {
            return "null";
        }
        return stringify_number(value.as_number());
    } else if (value.is_string()) {
        return stringify_string(value.to_string());
    } else if (value.is_object()) {
        const Object* obj = value.as_object();
        if (obj) {
            bool is_proxy = (obj->get_type() == Object::ObjectType::Proxy);

            if (!is_proxy) {
                // rawJSON objects: null prototype + own "rawJSON" property
                if (obj->get_prototype() == nullptr && const_cast<Object*>(obj)->has_own_property("rawJSON")) {
                    Value raw_str = const_cast<Object*>(obj)->get_property("rawJSON");
                    return raw_str.is_string() ? raw_str.to_string() : "null";
                }

                // BigInt wrapper object (Object(0n)): prototype is BigInt.prototype -> TypeError
                if (context_) {
                    Value bigint_ctor = context_->get_binding("BigInt");
                    if (bigint_ctor.is_function()) {
                        Value bigint_proto = bigint_ctor.as_function()->get_property("prototype");
                        if (bigint_proto.is_object() && obj->get_prototype() == bigint_proto.as_object()) {
                            context_->throw_type_error("Do not know how to serialize a BigInt");
                            return "";
                        }
                    }
                }
            }
            // Determine if it's array-like by unwrapping Proxies (Array.isArray semantics).
            const Object* unwrapped = obj;
            while (unwrapped && unwrapped->get_type() == Object::ObjectType::Proxy) {
                const Proxy* p = static_cast<const Proxy*>(unwrapped);
                if (p->is_revoked()) { unwrapped = nullptr; break; }
                unwrapped = p->get_proxy_target();
            }
            bool is_array_value = unwrapped && unwrapped->is_array();

            // toJSON takes the current key as its first argument
            if (obj && context_) {
                Value toJSON_val = const_cast<Object*>(obj)->get_property("toJSON");
                if (toJSON_val.is_function()) {
                    Function* toJSON_fn = toJSON_val.as_function();
                    std::vector<Value> args = { Value(current_key_) };
                    Value result = toJSON_fn->call(*context_, args, Value(const_cast<Object*>(obj)));
                    if (context_->has_exception()) return "";
                    if (result.is_undefined() || result.is_function() || result.is_symbol())
                        return "undefined_sentinel";
                    std::string saved = current_key_;
                    std::string r = stringify_value(result);
                    current_key_ = saved;
                    return r;
                }
            }

            // Boxed Boolean/Number/String: spec calls ToNumber(v) / ToString(v) which
            // invokes valueOf()/toString() on the object (can throw).
            if (obj && obj->is_primitive_wrapper() && context_) {
                auto otype = obj->get_type();
                if (otype == Object::ObjectType::Boolean) {
                    Value pv = const_cast<Object*>(obj)->get_property("[[PrimitiveValue]]");
                    return stringify_boolean(pv.to_boolean());
                } else if (otype == Object::ObjectType::Number) {
                    Value valueOf_fn = const_cast<Object*>(obj)->get_property("valueOf");
                    double n;
                    if (valueOf_fn.is_function()) {
                        Value r = valueOf_fn.as_function()->call(*context_, {}, value);
                        if (context_->has_exception()) return "";
                        n = r.to_number();
                    } else {
                        Value pv = const_cast<Object*>(obj)->get_property("[[PrimitiveValue]]");
                        n = pv.to_number();
                    }
                    if (std::isnan(n) || std::isinf(n)) return "null";
                    return stringify_number(n);
                } else if (otype == Object::ObjectType::String) {
                    Value toString_fn = const_cast<Object*>(obj)->get_property("toString");
                    if (toString_fn.is_function()) {
                        Value r = toString_fn.as_function()->call(*context_, {}, value);
                        if (context_->has_exception()) return "";
                        return stringify_string(r.to_string());
                    } else {
                        Value pv = const_cast<Object*>(obj)->get_property("[[PrimitiveValue]]");
                        return stringify_string(pv.to_string());
                    }
                }
            }

            if (is_array_value) {
                return stringify_array(obj);
            } else {
                return stringify_object(obj);
            }
        }
        return "null";
    } else {
        return "null";
    }
}

std::string JSON::Stringifier::stringify_object(const Object* obj) {
    if (!obj) return "null";

    if (visited_.find(obj) != visited_.end()) {
        throw std::runtime_error("TypeError: Converting circular structure to JSON");
    }

    visited_.insert(obj);
    depth_++;  // Increase depth for nested objects

    std::string result = "{";
    bool first = true;

    std::vector<std::string> keys;
    if (obj->get_type() == Object::ObjectType::Proxy) {
        Proxy* proxy = static_cast<Proxy*>(const_cast<Object*>(obj));
        if (!proxy->is_revoked()) {
            keys = proxy->own_keys_trap();
            // Filter to only enumerable string keys via the proxy's getOwnPropertyDescriptor trap.
            std::vector<std::string> enumerable_keys;
            for (const auto& k : keys) {
                if (k.find("@@sym:") == 0 || k.find("Symbol.") == 0) continue;
                PropertyDescriptor desc = proxy->get_own_property_descriptor_trap(Value(k));
                if (desc.is_data_descriptor() || desc.is_accessor_descriptor()) {
                    if (desc.is_enumerable()) enumerable_keys.push_back(k);
                }
            }
            keys = enumerable_keys;
        }
    } else {
        keys = obj->get_enumerable_keys();
    }

    // Build the list of keys to process.
    // If a replacer array is given (even empty), output follows replacer array order.
    std::vector<std::string> keys_to_process;
    if (options_.has_replacer_array) {
        // Emit keys in replacer-array order; only include keys present in the object.
        for (const std::string& ra_key : options_.replacer_array) {
            if (obj->get_type() == Object::ObjectType::Proxy ||
                std::find(keys.begin(), keys.end(), ra_key) != keys.end()) {
                keys_to_process.push_back(ra_key);
            }
        }
    } else {
        keys_to_process = keys;
    }

    // Check if this is a RegExp object (its own properties should be hidden from JSON)
    bool is_regexp_obj = (obj->get_type() == Object::ObjectType::RegExp);

    for (const std::string& key : keys_to_process) {
        if (key.substr(0, 2) == "__") continue;
        if (key.size() >= 2 && key[0] == '[' && key[1] == '[') continue;
        if (key.find("@@sym:") == 0 || key.find("Symbol.") == 0) continue;
        if (key.size() > 3 && key[0] == '_' && key[1] == 'i' && key[2] == 's') continue;
        // RegExp instances: hide implementation properties from JSON serialization
        if (is_regexp_obj && (key == "source" || key == "global" || key == "ignoreCase" ||
            key == "multiline" || key == "dotAll" || key == "unicode" ||
            key == "unicodeSets" || key == "sticky" || key == "flags" || key == "lastIndex" ||
            key == "__pattern__" || key == "__flags__")) continue;

        Value prop_value;
        if (obj->get_type() == Object::ObjectType::Proxy) {
            prop_value = static_cast<Proxy*>(const_cast<Object*>(obj))->get_trap(Value(key));
        } else {
            prop_value = obj->get_property(key);
        }

        // Apply toJSON BEFORE replacer (spec: toJSON called first, then replacer sees result)
        if (context_ && (prop_value.is_object() || prop_value.is_function())) {
            Object* pobj = prop_value.is_function() ? static_cast<Object*>(prop_value.as_function()) : prop_value.as_object();
            if (pobj) {
                Value tj = pobj->get_property("toJSON");
                if (tj.is_function()) {
                    Value r = tj.as_function()->call(*context_, {Value(key)}, prop_value);
                    if (context_->has_exception()) { visited_.erase(obj); depth_--; return ""; }
                    prop_value = r;
                }
            }
        }

        // If replacer function exists, call it
        if (options_.replacer_function && context_) {
            std::vector<Value> args;
            args.push_back(Value(key));
            args.push_back(prop_value);

            Value result_val = options_.replacer_function->call(*context_, args, Value(const_cast<Object*>(obj)));
            if (context_->has_exception()) {
                visited_.erase(obj);
                depth_--;
                return "";
            }
            prop_value = result_val;
        }

        if (prop_value.is_function() || prop_value.is_undefined() || prop_value.is_symbol()) continue;

        current_key_ = key;
        std::string serialized = stringify_value(prop_value);
        if (serialized == "undefined_sentinel") continue;
        if (context_ && context_->has_exception()) {
            visited_.erase(obj);
            depth_--;
            return "";
        }

        if (!first) result += ",";
        first = false;

        if (!options_.indent.empty()) {
            result += get_newline();
            result += get_indent();
        }

        result += stringify_string(key);
        result += ":";
        if (!options_.indent.empty()) result += " ";
        result += serialized;
    }

    if (!options_.indent.empty() && !first) {
        result += get_newline();
        if (depth_ > 0) {
            // Repeat the indent string (depth_ - 1) times
            for (size_t i = 0; i < depth_ - 1; i++) {
                result += options_.indent;
            }
        }
    }

    result += "}";
    visited_.erase(obj);
    depth_--;  // Decrease depth after object
    return result;
}

std::string JSON::Stringifier::stringify_array(const Object* arr) {
    if (!arr) return "null";

    if (visited_.find(arr) != visited_.end()) {
        throw std::runtime_error("TypeError: Converting circular structure to JSON");
    }

    visited_.insert(arr);
    depth_++;  // Increase depth for nested arrays

    std::string result = "[";
    bool first = true;

    Value length_val = arr->get_property("length");
    uint32_t length = static_cast<uint32_t>(length_val.to_number());
    
    for (uint32_t i = 0; i < length; i++) {
        if (!first) {
            result += ",";
        }
        first = false;
        
        if (!options_.indent.empty()) {
            result += get_newline();
            result += get_indent();
        }
        
        Value element = arr->get_element(i);
        current_key_ = std::to_string(i);
        if (options_.replacer_function && context_) {
            std::vector<Value> rargs = { Value(current_key_), element };
            Value rresult = options_.replacer_function->call(*context_, rargs, Value(const_cast<Object*>(arr)));
            if (context_->has_exception()) { visited_.erase(arr); depth_--; return ""; }
            element = rresult;
        }
        if (element.is_undefined() || element.is_function() || element.is_symbol()) element = Value();
        std::string serialized = stringify_value(element);
        if (serialized == "undefined_sentinel") serialized = "null";
        result += serialized;
    }
    
    if (!options_.indent.empty() && !first) {
        result += get_newline();
        if (depth_ > 0) {
            // Repeat the indent string (depth_ - 1) times
            for (size_t i = 0; i < depth_ - 1; i++) {
                result += options_.indent;
            }
        }
    }

    result += "]";
    visited_.erase(arr);
    depth_--;  // Decrease depth after array
    return result;
}

std::string JSON::Stringifier::stringify_string(const std::string& str) {
    return "\"" + escape_string(str) + "\"";
}

std::string JSON::Stringifier::stringify_number(double num) {
    if (num == 0.0) return "0";
    std::ostringstream oss;
    oss << num;
    return oss.str();
}

std::string JSON::Stringifier::stringify_boolean(bool value) {
    return value ? "true" : "false";
}

std::string JSON::Stringifier::get_indent() const {
    if (options_.indent.empty()) return "";
    // Repeat the indent string depth_ times
    std::string result;
    for (size_t i = 0; i < depth_; i++) {
        result += options_.indent;
    }
    return result;
}

std::string JSON::Stringifier::get_newline() const {
    return options_.indent.empty() ? "" : "\n";
}

static void append_unicode_escape(std::string& result, uint32_t cp) {
    static const char hex[] = "0123456789abcdef";
    result += "\\u";
    result += hex[(cp >> 12) & 0xF];
    result += hex[(cp >> 8) & 0xF];
    result += hex[(cp >> 4) & 0xF];
    result += hex[cp & 0xF];
}

std::string JSON::Stringifier::escape_string(const std::string& str) {
    std::string result;
    result.reserve(str.length());

    size_t i = 0;
    while (i < str.size()) {
        unsigned char b = static_cast<unsigned char>(str[i]);

        if (b < 0x80) {
            char ch = static_cast<char>(b);
            switch (ch) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (b < 0x20) {
                        append_unicode_escape(result, b);
                    } else {
                        result += ch;
                    }
                    break;
            }
            i++;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < str.size()) {
            unsigned char b2 = static_cast<unsigned char>(str[i + 1]);
            result += static_cast<char>(b);
            result += static_cast<char>(b2);
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < str.size()) {
            unsigned char b2 = static_cast<unsigned char>(str[i + 1]);
            unsigned char b3 = static_cast<unsigned char>(str[i + 2]);
            uint32_t cp = ((b & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                append_unicode_escape(result, cp);
            } else {
                result += static_cast<char>(b);
                result += static_cast<char>(b2);
                result += static_cast<char>(b3);
            }
            i += 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < str.size()) {
            result += static_cast<char>(b);
            result += static_cast<char>(str[i + 1]);
            result += static_cast<char>(str[i + 2]);
            result += static_cast<char>(str[i + 3]);
            i += 4;
        } else {
            result += static_cast<char>(b);
            i++;
        }
    }

    return result;
}


bool JSON::is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

}
