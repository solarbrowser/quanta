/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/json_parser.h"
#include "../../include/Object.h"
#include "../../include/Context.h"
#include <iostream>
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>

namespace Quanta {

JSONParser::JSONParser(bool strict)
    : input_(nullptr), position_(0), length_(0), strict_mode_(strict) {
}

JSONParser::ParseResult JSONParser::parse(const std::string& json_string) {
    return parse(json_string.c_str(), json_string.length());
}

JSONParser::ParseResult JSONParser::parse(const char* json_data, size_t length) {
    input_ = json_data;
    position_ = 0;
    length_ = length;

    ParseResult result;

    try {
        skip_whitespace();
        if (position_ >= length_) {
            return ParseResult(ParseError::UnexpectedToken, position_, "Unexpected end of input");
        }

        result.value = parse_value();

        skip_whitespace();
        if (position_ < length_) {
            return ParseResult(ParseError::UnexpectedToken, position_, "Unexpected token after JSON");
        }

        result.error = ParseError::None;
    } catch (const std::exception& e) {
        result.error = ParseError::UnexpectedToken;
        result.error_message = e.what();
        result.error_position = position_;
    }

    return result;
}

Value JSONParser::parse_value() {
    skip_whitespace();

    if (position_ >= length_) {
        throw std::runtime_error("Unexpected end of input");
    }

    char c = peek_char();

    switch (c) {
        case '"':
            return parse_string();
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case 't':
        case 'f':
        case 'n':
            return parse_literal();
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return parse_number();
        default:
            throw std::runtime_error("Unexpected character: " + std::string(1, c));
    }
}

Value JSONParser::parse_object() {
    if (consume_char() != '{') {
        throw std::runtime_error("Expected '{'");
    }

    auto obj = new Object();
    skip_whitespace();

    // Empty object
    if (peek_char() == '}') {
        consume_char();
        return Value(obj);
    }

    while (true) {
        skip_whitespace();

        // Parse key
        if (peek_char() != '"') {
            throw std::runtime_error("Expected string key");
        }

        Value key_value = parse_string();
        std::string key = key_value.to_string();

        skip_whitespace();
        if (consume_char() != ':') {
            throw std::runtime_error("Expected ':'");
        }

        skip_whitespace();
        Value value = parse_value();

        obj->set_property(key, value);

        skip_whitespace();
        char next = peek_char();

        if (next == '}') {
            consume_char();
            break;
        } else if (next == ',') {
            consume_char();
            skip_whitespace();

            // Check for trailing comma
            if (strict_mode_ && peek_char() == '}') {
                throw std::runtime_error("Trailing comma in object");
            }
        } else {
            throw std::runtime_error("Expected ',' or '}'");
        }
    }

    return Value(obj);
}

Value JSONParser::parse_array() {
    if (consume_char() != '[') {
        throw std::runtime_error("Expected '['");
    }

    auto arr = new Object(); // Array-like object
    arr->set_property("length", Value(0.0));

    skip_whitespace();

    // Empty array
    if (peek_char() == ']') {
        consume_char();
        return Value(arr);
    }

    size_t index = 0;

    while (true) {
        skip_whitespace();
        Value value = parse_value();

        arr->set_property(std::to_string(index), value);
        index++;

        skip_whitespace();
        char next = peek_char();

        if (next == ']') {
            consume_char();
            break;
        } else if (next == ',') {
            consume_char();
            skip_whitespace();

            // Check for trailing comma
            if (strict_mode_ && peek_char() == ']') {
                throw std::runtime_error("Trailing comma in array");
            }
        } else {
            throw std::runtime_error("Expected ',' or ']'");
        }
    }

    arr->set_property("length", Value(static_cast<double>(index)));
    return Value(arr);
}

Value JSONParser::parse_string() {
    if (consume_char() != '"') {
        throw std::runtime_error("Expected '\"'");
    }

    std::string result;

    while (position_ < length_) {
        char c = consume_char();

        if (c == '"') {
            return Value(result);
        } else if (c == '\\') {
            if (position_ >= length_) {
                throw std::runtime_error("Unterminated string escape");
            }

            char escape = consume_char();
            switch (escape) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    // Unicode escape sequence
                    if (position_ + 4 > length_) {
                        throw std::runtime_error("Invalid unicode escape");
                    }

                    std::string hex_str(input_ + position_, 4);
                    position_ += 4;

                    // Convert hex to character (simplified)
                    uint16_t code_point = std::stoul(hex_str, nullptr, 16);
                    if (code_point < 128) {
                        result += static_cast<char>(code_point);
                    } else {
                        // For simplicity, append as hex for non-ASCII
                        result += "\\u" + hex_str;
                    }
                    break;
                }
                default:
                    throw std::runtime_error("Invalid escape sequence");
            }
        } else if (c < 0x20) {
            throw std::runtime_error("Unescaped control character in string");
        } else {
            result += c;
        }
    }

    throw std::runtime_error("Unterminated string");
}

Value JSONParser::parse_number() {
    size_t start = position_;

    // Optional minus
    if (peek_char() == '-') {
        consume_char();
    }

    // Integer part
    if (peek_char() == '0') {
        consume_char();
    } else if (std::isdigit(peek_char())) {
        while (position_ < length_ && std::isdigit(peek_char())) {
            consume_char();
        }
    } else {
        throw std::runtime_error("Invalid number");
    }

    // Fractional part
    if (position_ < length_ && peek_char() == '.') {
        consume_char();
        if (!std::isdigit(peek_char())) {
            throw std::runtime_error("Invalid number: expected digit after '.'");
        }
        while (position_ < length_ && std::isdigit(peek_char())) {
            consume_char();
        }
    }

    // Exponent part
    if (position_ < length_ && (peek_char() == 'e' || peek_char() == 'E')) {
        consume_char();
        if (position_ < length_ && (peek_char() == '+' || peek_char() == '-')) {
            consume_char();
        }
        if (!std::isdigit(peek_char())) {
            throw std::runtime_error("Invalid number: expected digit in exponent");
        }
        while (position_ < length_ && std::isdigit(peek_char())) {
            consume_char();
        }
    }

    std::string number_str(input_ + start, position_ - start);
    double value = std::stod(number_str);

    return Value(value);
}

Value JSONParser::parse_literal() {
    if (match_string("true")) {
        return Value(true);
    } else if (match_string("false")) {
        return Value(false);
    } else if (match_string("null")) {
        return Value(); // null
    } else {
        throw std::runtime_error("Invalid literal");
    }
}

void JSONParser::skip_whitespace() {
    while (position_ < length_) {
        char c = input_[position_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            position_++;
        } else {
            break;
        }
    }
}

char JSONParser::peek_char() const {
    if (position_ >= length_) {
        return '\0';
    }
    return input_[position_];
}

char JSONParser::consume_char() {
    if (position_ >= length_) {
        return '\0';
    }
    return input_[position_++];
}

bool JSONParser::match_string(const char* str) {
    size_t len = strlen(str);
    if (position_ + len > length_) {
        return false;
    }

    if (strncmp(input_ + position_, str, len) == 0) {
        position_ += len;
        return true;
    }

    return false;
}

JSONParser::ParseError JSONParser::set_error(ParseError error, const std::string& message) {
    // This would be used to set error state in a more complex implementation
    return error;
}

std::string JSONParser::stringify(const Value& value, bool pretty) {
    return stringify_value(value, pretty ? 0 : -1);
}

std::string JSONParser::stringify(const Value& value, const Value& replacer, const Value& space) {
    // Simplified implementation - ignore replacer and space for now
    return stringify(value, !space.is_null());
}

std::string JSONParser::stringify_value(const Value& value, int indent_level) {
    if (value.is_null()) {
        return "null";
    } else if (value.is_boolean()) {
        return value.to_boolean() ? "true" : "false";
    } else if (value.is_number()) {
        double num = value.to_number();
        if (std::isnan(num) || std::isinf(num)) {
            return "null";
        }

        // Check if it's an integer
        if (std::floor(num) == num && std::abs(num) < 1e15) {
            return std::to_string(static_cast<long long>(num));
        } else {
            return std::to_string(num);
        }
    } else if (value.is_string()) {
        return stringify_string(value.to_string());
    } else if (value.is_object()) {
        Object* obj = value.to_object();
        if (!obj) {
            return "null";
        }

        // Check if it's an array (has numeric indices and length property)
        Value length_val = obj->get_property("length");
        if (!length_val.is_null() && length_val.is_number()) {
            return stringify_array(obj, indent_level);
        } else {
            return stringify_object(obj, indent_level);
        }
    }

    return "null";
}

std::string JSONParser::stringify_object(Object* obj, int indent_level) {
    std::string result = "{";
    bool first = true;
    bool pretty = (indent_level >= 0);

    if (pretty) {
        result += "\n";
    }

    // In a real implementation, we would iterate over object properties
    // For now, this is a simplified version

    if (pretty) {
        result += create_indent(indent_level);
    }
    result += "}";

    return result;
}

std::string JSONParser::stringify_array(Object* arr, int indent_level) {
    std::string result = "[";
    bool first = true;
    bool pretty = (indent_level >= 0);

    Value length_val = arr->get_property("length");
    size_t length = static_cast<size_t>(length_val.to_number());

    if (pretty && length > 0) {
        result += "\n";
    }

    for (size_t i = 0; i < length; ++i) {
        if (!first) {
            result += ",";
            if (pretty) {
                result += "\n";
            }
        }
        first = false;

        if (pretty) {
            result += create_indent(indent_level + 1);
        }

        Value element = arr->get_property(std::to_string(i));
        result += stringify_value(element, pretty ? indent_level + 1 : -1);
    }

    if (pretty && length > 0) {
        result += "\n" + create_indent(indent_level);
    }
    result += "]";

    return result;
}

std::string JSONParser::stringify_string(const std::string& str) {
    return escape_string(str);
}

std::string JSONParser::escape_string(const std::string& str) {
    std::string result = "\"";

    for (char c : str) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (c < 0x20) {
                    // Escape control characters
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<int>(c);
                    result += oss.str();
                } else {
                    result += c;
                }
                break;
        }
    }

    result += "\"";
    return result;
}

std::string JSONParser::create_indent(int level) {
    return std::string(level * 2, ' '); // 2 spaces per indent level
}

// Static methods for JavaScript binding
Value JSONParser::json_parse(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        // TODO: Throw SyntaxError
        return Value();
    }

    std::string json_str = args[0].to_string();
    JSONParser parser;
    ParseResult result = parser.parse(json_str);

    if (result.error != ParseError::None) {
        // TODO: Throw SyntaxError with message
        return Value();
    }

    return result.value;
}

Value JSONParser::json_stringify(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(); // undefined
    }

    JSONParser parser;
    bool pretty = args.size() > 2 && !args[2].is_null();

    std::string result = parser.stringify(args[0], pretty);
    return Value(result);
}

void JSONParser::setup_json_object(Context& ctx) {
    // Set up global JSON object with parse and stringify methods
    // This would be called during engine initialization
}

} // namespace Quanta