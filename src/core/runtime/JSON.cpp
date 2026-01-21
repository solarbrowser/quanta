/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/JSON.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Error.h"
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

std::string JSON::stringify(const Value& value, const StringifyOptions& options) {
    Stringifier stringifier(options, nullptr);
    return stringifier.stringify(value);
}

Value JSON::js_parse(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_syntax_error("JSON.parse requires at least 1 argument");
        return Value();
    }
    
    std::string json_string = args[0].to_string();
    
    if (json_string.empty()) {
        ctx.throw_syntax_error("JSON.parse: unexpected end of input");
        return Value();
    }
    
    try {
        return parse(json_string);
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
    
    if (args[0].is_undefined()) {
        return Value();
    }
    
    if (args[0].is_object() && !args[0].as_object()) {
        return Value("null");
    }
    
    StringifyOptions options;
    
    if (args.size() > 2 && !args[2].is_undefined()) {
        if (args[2].is_number()) {
            int indent_count = static_cast<int>(args[2].to_number());
            if (indent_count > 0) {
                options.indent = std::string(std::min(indent_count, 10), ' ');
            }
        } else if (args[2].is_string()) {
            options.indent = args[2].to_string().substr(0, 10);
        }
    }
    
    try {
        Stringifier stringifier(options, &ctx);
        std::string result = stringifier.stringify(args[0]);
        return Value(result);
    } catch (const std::exception& e) {
        ctx.throw_type_error(std::string(e.what()));
        return Value();
    }
}

std::unique_ptr<Object> JSON::create_json_object() {
    auto json_obj = std::make_unique<Object>();
    
    json_obj->set_property("parse", Value("function JSON.parse() { [native code] }"));
    json_obj->set_property("stringify", Value("function JSON.stringify() { [native code] }"));
    
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
    
    Value result = parse_value();
    
    skip_whitespace();
    if (!at_end()) {
        throw_syntax_error("Unexpected token after JSON value");
    }
    
    return result;
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
    
    auto obj = std::make_unique<Object>();
    
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
        
        Value value = parse_value();
        obj->set_property(key, value);
        
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
        Value value = parse_value();
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
    
    return std::stod(number_str);
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
    : options_(options), depth_(0), context_(ctx) {
}

std::string JSON::Stringifier::stringify(const Value& value) {
    return stringify_value(value);
}

std::string JSON::Stringifier::stringify_value(const Value& value) {
    if (value.is_null()) {
        return "null";
    } else if (value.is_undefined()) {
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
            if (obj->has_property("toJSON") && context_) {
                Value toJSON_val = obj->get_property("toJSON");
                if (toJSON_val.is_function()) {
                    Function* toJSON_fn = toJSON_val.as_function();
                    std::vector<Value> args;
                    Value result = toJSON_fn->call(*context_, args, Value(const_cast<Object*>(obj)));
                    if (!context_->has_exception()) {
                        return stringify_value(result);
                    }
                }
            }

            if (obj->is_array()) {
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

    std::string result = "{";
    bool first = true;

    std::vector<std::string> keys = obj->get_enumerable_keys();
    
    for (const std::string& key : keys) {
        if (key.substr(0, 2) == "__") continue;
        
        Value prop_value = obj->get_property(key);
        
        if (prop_value.is_function() || prop_value.is_undefined()) continue;
        
        if (!first) {
            result += ",";
        }
        first = false;
        
        if (!options_.indent.empty()) {
            result += get_newline();
            result += get_indent();
        }
        
        result += stringify_string(key);
        result += ":";
        
        if (!options_.indent.empty()) {
            result += " ";
        }
        
        result += stringify_value(prop_value);
    }
    
    if (!options_.indent.empty() && !first) {
        result += get_newline();
        if (depth_ > 0) {
            result += std::string((depth_ - 1) * options_.indent.length(), ' ');
        }
    }

    result += "}";
    visited_.erase(obj);
    return result;
}

std::string JSON::Stringifier::stringify_array(const Object* arr) {
    if (!arr) return "null";

    if (visited_.find(arr) != visited_.end()) {
        throw std::runtime_error("TypeError: Converting circular structure to JSON");
    }

    visited_.insert(arr);

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
        result += stringify_value(element);
    }
    
    if (!options_.indent.empty() && !first) {
        result += get_newline();
        if (depth_ > 0) {
            result += std::string((depth_ - 1) * options_.indent.length(), ' ');
        }
    }

    result += "]";
    visited_.erase(arr);
    return result;
}

std::string JSON::Stringifier::stringify_string(const std::string& str) {
    return "\"" + escape_string(str) + "\"";
}

std::string JSON::Stringifier::stringify_number(double num) {
    std::ostringstream oss;
    oss << num;
    return oss.str();
}

std::string JSON::Stringifier::stringify_boolean(bool value) {
    return value ? "true" : "false";
}

std::string JSON::Stringifier::get_indent() const {
    if (options_.indent.empty()) return "";
    return std::string(depth_ * options_.indent.length(), ' ');
}

std::string JSON::Stringifier::get_newline() const {
    return options_.indent.empty() ? "" : "\n";
}

std::string JSON::Stringifier::escape_string(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (char ch : str) {
        switch (ch) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += "\\u";
                    result += std::to_string((ch >> 12) & 0xF);
                    result += std::to_string((ch >> 8) & 0xF);
                    result += std::to_string((ch >> 4) & 0xF);
                    result += std::to_string(ch & 0xF);
                } else {
                    result += ch;
                }
                break;
        }
    }
    
    return result;
}


bool JSON::is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

}
