#include "../include/AST.h"
#include "../../core/include/Context.h"
#include "../../core/include/Engine.h"
#include "../../core/include/Object.h"
#include <sstream>
#include <iostream>

namespace Quanta {

//=============================================================================
// NumberLiteral Implementation
//=============================================================================

Value NumberLiteral::evaluate(Context& ctx) {
    (void)ctx; // Suppress unused parameter warning
    return Value(value_);
}

std::string NumberLiteral::to_string() const {
    return std::to_string(value_);
}

std::unique_ptr<ASTNode> NumberLiteral::clone() const {
    return std::make_unique<NumberLiteral>(value_, start_, end_);
}

//=============================================================================
// StringLiteral Implementation
//=============================================================================

Value StringLiteral::evaluate(Context& ctx) {
    (void)ctx; // Suppress unused parameter warning
    return Value(value_);
}

std::string StringLiteral::to_string() const {
    return "\"" + value_ + "\"";
}

std::unique_ptr<ASTNode> StringLiteral::clone() const {
    return std::make_unique<StringLiteral>(value_, start_, end_);
}

//=============================================================================
// BooleanLiteral Implementation
//=============================================================================

Value BooleanLiteral::evaluate(Context& ctx) {
    (void)ctx; // Suppress unused parameter warning
    return Value(value_);
}

std::string BooleanLiteral::to_string() const {
    return value_ ? "true" : "false";
}

std::unique_ptr<ASTNode> BooleanLiteral::clone() const {
    return std::make_unique<BooleanLiteral>(value_, start_, end_);
}

//=============================================================================
// NullLiteral Implementation
//=============================================================================

Value NullLiteral::evaluate(Context& ctx) {
    (void)ctx; // Suppress unused parameter warning
    return Value::null();
}

std::string NullLiteral::to_string() const {
    return "null";
}

std::unique_ptr<ASTNode> NullLiteral::clone() const {
    return std::make_unique<NullLiteral>(start_, end_);
}

//=============================================================================
// UndefinedLiteral Implementation
//=============================================================================

Value UndefinedLiteral::evaluate(Context& ctx) {
    (void)ctx; // Suppress unused parameter warning
    return Value();
}

std::string UndefinedLiteral::to_string() const {
    return "undefined";
}

std::unique_ptr<ASTNode> UndefinedLiteral::clone() const {
    return std::make_unique<UndefinedLiteral>(start_, end_);
}

//=============================================================================
// Identifier Implementation
//=============================================================================

Value Identifier::evaluate(Context& ctx) {
    return ctx.get_binding(name_);
}

std::string Identifier::to_string() const {
    return name_;
}

std::unique_ptr<ASTNode> Identifier::clone() const {
    return std::make_unique<Identifier>(name_, start_, end_);
}

//=============================================================================
// BinaryExpression Implementation
//=============================================================================

Value BinaryExpression::evaluate(Context& ctx) {
    // Handle assignment operators specially
    if (operator_ == Operator::ASSIGN || 
        operator_ == Operator::PLUS_ASSIGN ||
        operator_ == Operator::MINUS_ASSIGN ||
        operator_ == Operator::MULTIPLY_ASSIGN ||
        operator_ == Operator::DIVIDE_ASSIGN ||
        operator_ == Operator::MODULO_ASSIGN) {
        
        Value right_value = right_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        // For compound assignments, we need the current value first
        Value result_value = right_value;
        if (operator_ != Operator::ASSIGN) {
            Value left_value = left_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            // Perform the compound operation
            switch (operator_) {
                case Operator::PLUS_ASSIGN:
                    result_value = left_value.add(right_value);
                    break;
                case Operator::MINUS_ASSIGN:
                    result_value = left_value.subtract(right_value);
                    break;
                case Operator::MULTIPLY_ASSIGN:
                    result_value = left_value.multiply(right_value);
                    break;
                case Operator::DIVIDE_ASSIGN:
                    result_value = left_value.divide(right_value);
                    break;
                case Operator::MODULO_ASSIGN:
                    result_value = left_value.modulo(right_value);
                    break;
                default:
                    break; // ASSIGN case handled below
            }
        }
        
        // Support identifier assignment
        if (left_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            ctx.set_binding(id->get_name(), result_value);
            return result_value;
        }
        
        // Support member expression assignment (obj.prop = value)
        if (left_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            MemberExpression* member = static_cast<MemberExpression*>(left_.get());
            
            // Evaluate the object
            Value object_value = member->get_object()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            // Check if it's an object
            if (object_value.is_object()) {
                Object* obj = object_value.as_object();
                
                // Get the property key
                std::string key;
                if (member->is_computed()) {
                    // For obj[expr] = value
                    Value key_value = member->get_property()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    key = key_value.to_string();
                } else {
                    // For obj.prop = value
                    if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                        Identifier* prop = static_cast<Identifier*>(member->get_property());
                        key = prop->get_name();
                    } else {
                        ctx.throw_exception(Value("Invalid property in assignment"));
                        return Value();
                    }
                }
                
                // Set the property
                obj->set_property(key, result_value);
                return result_value;
            } else {
                ctx.throw_exception(Value("Cannot set property on non-object"));
                return Value();
            }
        }
        
        ctx.throw_exception(Value("Invalid left-hand side in assignment"));
        return Value();
    }
    
    // Evaluate operands
    Value left_value = left_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    // Short-circuit evaluation for logical operators
    if (operator_ == Operator::LOGICAL_AND) {
        if (!left_value.to_boolean()) {
            return left_value;
        }
        return right_->evaluate(ctx);
    }
    
    if (operator_ == Operator::LOGICAL_OR) {
        if (left_value.to_boolean()) {
            return left_value;
        }
        return right_->evaluate(ctx);
    }
    
    Value right_value = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    // Perform operation based on operator
    switch (operator_) {
        case Operator::ADD:
            return left_value.add(right_value);
        case Operator::SUBTRACT:
            return left_value.subtract(right_value);
        case Operator::MULTIPLY:
            return left_value.multiply(right_value);
        case Operator::DIVIDE:
            return left_value.divide(right_value);
        case Operator::MODULO:
            return left_value.modulo(right_value);
        case Operator::EXPONENT:
            return left_value.power(right_value);
            
        case Operator::EQUAL:
            return Value(left_value.loose_equals(right_value));
        case Operator::NOT_EQUAL:
            return Value(!left_value.loose_equals(right_value));
        case Operator::STRICT_EQUAL:
            return Value(left_value.strict_equals(right_value));
        case Operator::STRICT_NOT_EQUAL:
            return Value(!left_value.strict_equals(right_value));
        case Operator::LESS_THAN:
            return Value(left_value.compare(right_value) < 0);
        case Operator::GREATER_THAN:
            return Value(left_value.compare(right_value) > 0);
        case Operator::LESS_EQUAL:
            return Value(left_value.compare(right_value) <= 0);
        case Operator::GREATER_EQUAL:
            return Value(left_value.compare(right_value) >= 0);
            
        case Operator::BITWISE_AND:
            return left_value.bitwise_and(right_value);
        case Operator::BITWISE_OR:
            return left_value.bitwise_or(right_value);
        case Operator::BITWISE_XOR:
            return left_value.bitwise_xor(right_value);
        case Operator::LEFT_SHIFT:
            return left_value.left_shift(right_value);
        case Operator::RIGHT_SHIFT:
            return left_value.right_shift(right_value);
        case Operator::UNSIGNED_RIGHT_SHIFT:
            return left_value.unsigned_right_shift(right_value);
            
        default:
            ctx.throw_exception(Value("Unsupported binary operator"));
            return Value();
    }
}

std::string BinaryExpression::to_string() const {
    return "(" + left_->to_string() + " " + operator_to_string(operator_) + " " + right_->to_string() + ")";
}

std::unique_ptr<ASTNode> BinaryExpression::clone() const {
    return std::make_unique<BinaryExpression>(
        left_->clone(), operator_, right_->clone(), start_, end_
    );
}

std::string BinaryExpression::operator_to_string(Operator op) {
    switch (op) {
        case Operator::ADD: return "+";
        case Operator::SUBTRACT: return "-";
        case Operator::MULTIPLY: return "*";
        case Operator::DIVIDE: return "/";
        case Operator::MODULO: return "%";
        case Operator::EXPONENT: return "**";
        case Operator::ASSIGN: return "=";
        case Operator::PLUS_ASSIGN: return "+=";
        case Operator::MINUS_ASSIGN: return "-=";
        case Operator::MULTIPLY_ASSIGN: return "*=";
        case Operator::DIVIDE_ASSIGN: return "/=";
        case Operator::MODULO_ASSIGN: return "%=";
        case Operator::EQUAL: return "==";
        case Operator::NOT_EQUAL: return "!=";
        case Operator::STRICT_EQUAL: return "===";
        case Operator::STRICT_NOT_EQUAL: return "!==";
        case Operator::LESS_THAN: return "<";
        case Operator::GREATER_THAN: return ">";
        case Operator::LESS_EQUAL: return "<=";
        case Operator::GREATER_EQUAL: return ">=";
        case Operator::LOGICAL_AND: return "&&";
        case Operator::LOGICAL_OR: return "||";
        case Operator::BITWISE_AND: return "&";
        case Operator::BITWISE_OR: return "|";
        case Operator::BITWISE_XOR: return "^";
        case Operator::LEFT_SHIFT: return "<<";
        case Operator::RIGHT_SHIFT: return ">>";
        case Operator::UNSIGNED_RIGHT_SHIFT: return ">>>";
        default: return "?";
    }
}

BinaryExpression::Operator BinaryExpression::token_type_to_operator(TokenType type) {
    switch (type) {
        case TokenType::PLUS: return Operator::ADD;
        case TokenType::MINUS: return Operator::SUBTRACT;
        case TokenType::MULTIPLY: return Operator::MULTIPLY;
        case TokenType::DIVIDE: return Operator::DIVIDE;
        case TokenType::MODULO: return Operator::MODULO;
        case TokenType::EXPONENT: return Operator::EXPONENT;
        case TokenType::ASSIGN: return Operator::ASSIGN;
        case TokenType::PLUS_ASSIGN: return Operator::PLUS_ASSIGN;
        case TokenType::MINUS_ASSIGN: return Operator::MINUS_ASSIGN;
        case TokenType::MULTIPLY_ASSIGN: return Operator::MULTIPLY_ASSIGN;
        case TokenType::DIVIDE_ASSIGN: return Operator::DIVIDE_ASSIGN;
        case TokenType::MODULO_ASSIGN: return Operator::MODULO_ASSIGN;
        case TokenType::EQUAL: return Operator::EQUAL;
        case TokenType::NOT_EQUAL: return Operator::NOT_EQUAL;
        case TokenType::STRICT_EQUAL: return Operator::STRICT_EQUAL;
        case TokenType::STRICT_NOT_EQUAL: return Operator::STRICT_NOT_EQUAL;
        case TokenType::LESS_THAN: return Operator::LESS_THAN;
        case TokenType::GREATER_THAN: return Operator::GREATER_THAN;
        case TokenType::LESS_EQUAL: return Operator::LESS_EQUAL;
        case TokenType::GREATER_EQUAL: return Operator::GREATER_EQUAL;
        case TokenType::LOGICAL_AND: return Operator::LOGICAL_AND;
        case TokenType::LOGICAL_OR: return Operator::LOGICAL_OR;
        case TokenType::BITWISE_AND: return Operator::BITWISE_AND;
        case TokenType::BITWISE_OR: return Operator::BITWISE_OR;
        case TokenType::BITWISE_XOR: return Operator::BITWISE_XOR;
        case TokenType::LEFT_SHIFT: return Operator::LEFT_SHIFT;
        case TokenType::RIGHT_SHIFT: return Operator::RIGHT_SHIFT;
        case TokenType::UNSIGNED_RIGHT_SHIFT: return Operator::UNSIGNED_RIGHT_SHIFT;
        default: return Operator::ADD; // fallback
    }
}

int BinaryExpression::get_precedence(Operator op) {
    switch (op) {
        case Operator::ASSIGN: return 1;
        case Operator::LOGICAL_OR: return 2;
        case Operator::LOGICAL_AND: return 3;
        case Operator::BITWISE_OR: return 4;
        case Operator::BITWISE_XOR: return 5;
        case Operator::BITWISE_AND: return 6;
        case Operator::EQUAL:
        case Operator::NOT_EQUAL:
        case Operator::STRICT_EQUAL:
        case Operator::STRICT_NOT_EQUAL: return 7;
        case Operator::LESS_THAN:
        case Operator::GREATER_THAN:
        case Operator::LESS_EQUAL:
        case Operator::GREATER_EQUAL: return 8;
        case Operator::LEFT_SHIFT:
        case Operator::RIGHT_SHIFT:
        case Operator::UNSIGNED_RIGHT_SHIFT: return 9;
        case Operator::ADD:
        case Operator::SUBTRACT: return 10;
        case Operator::MULTIPLY:
        case Operator::DIVIDE:
        case Operator::MODULO: return 11;
        case Operator::EXPONENT: return 12;
        default: return 0;
    }
}

bool BinaryExpression::is_right_associative(Operator op) {
    return op == Operator::ASSIGN || op == Operator::EXPONENT;
}

//=============================================================================
// UnaryExpression Implementation
//=============================================================================

Value UnaryExpression::evaluate(Context& ctx) {
    switch (operator_) {
        case Operator::PLUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.unary_plus();
        }
        case Operator::MINUS: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.unary_minus();
        }
        case Operator::LOGICAL_NOT: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.logical_not();
        }
        case Operator::BITWISE_NOT: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.bitwise_not();
        }
        case Operator::TYPEOF: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return operand_value.typeof_op();
        }
        case Operator::VOID: {
            Value operand_value = operand_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return Value(); // void always returns undefined
        }
        case Operator::PRE_INCREMENT: {
            // For ++x, increment first then return new value
            if (operand_->get_type() != ASTNode::Type::IDENTIFIER) {
                ctx.throw_exception(Value("Invalid left-hand side in assignment"));
                return Value();
            }
            Identifier* id = static_cast<Identifier*>(operand_.get());
            Value current = ctx.get_binding(id->get_name());
            Value incremented = Value(current.to_number() + 1.0);
            ctx.set_binding(id->get_name(), incremented);
            return incremented;
        }
        case Operator::POST_INCREMENT: {
            // For x++, return old value then increment
            if (operand_->get_type() != ASTNode::Type::IDENTIFIER) {
                ctx.throw_exception(Value("Invalid left-hand side in assignment"));
                return Value();
            }
            Identifier* id = static_cast<Identifier*>(operand_.get());
            Value current = ctx.get_binding(id->get_name());
            Value incremented = Value(current.to_number() + 1.0);
            ctx.set_binding(id->get_name(), incremented);
            return current; // return original value
        }
        case Operator::PRE_DECREMENT: {
            // For --x, decrement first then return new value
            if (operand_->get_type() != ASTNode::Type::IDENTIFIER) {
                ctx.throw_exception(Value("Invalid left-hand side in assignment"));
                return Value();
            }
            Identifier* id = static_cast<Identifier*>(operand_.get());
            Value current = ctx.get_binding(id->get_name());
            Value decremented = Value(current.to_number() - 1.0);
            ctx.set_binding(id->get_name(), decremented);
            return decremented;
        }
        case Operator::POST_DECREMENT: {
            // For x--, return old value then decrement
            if (operand_->get_type() != ASTNode::Type::IDENTIFIER) {
                ctx.throw_exception(Value("Invalid left-hand side in assignment"));
                return Value();
            }
            Identifier* id = static_cast<Identifier*>(operand_.get());
            Value current = ctx.get_binding(id->get_name());
            Value decremented = Value(current.to_number() - 1.0);
            ctx.set_binding(id->get_name(), decremented);
            return current; // return original value
        }
        default:
            ctx.throw_exception(Value("Unsupported unary operator"));
            return Value();
    }
}

std::string UnaryExpression::to_string() const {
    if (prefix_) {
        return operator_to_string(operator_) + operand_->to_string();
    } else {
        return operand_->to_string() + operator_to_string(operator_);
    }
}

std::unique_ptr<ASTNode> UnaryExpression::clone() const {
    return std::make_unique<UnaryExpression>(operator_, operand_->clone(), prefix_, start_, end_);
}

std::string UnaryExpression::operator_to_string(Operator op) {
    switch (op) {
        case Operator::PLUS: return "+";
        case Operator::MINUS: return "-";
        case Operator::LOGICAL_NOT: return "!";
        case Operator::BITWISE_NOT: return "~";
        case Operator::TYPEOF: return "typeof ";
        case Operator::VOID: return "void ";
        case Operator::DELETE: return "delete ";
        case Operator::PRE_INCREMENT: return "++";
        case Operator::POST_INCREMENT: return "++";
        case Operator::PRE_DECREMENT: return "--";
        case Operator::POST_DECREMENT: return "--";
        default: return "?";
    }
}

//=============================================================================
// CallExpression Implementation
//=============================================================================

Value CallExpression::evaluate(Context& ctx) {
    // First, try to evaluate callee as a function
    Value callee_value = callee_->evaluate(ctx);
    
    if (callee_value.is_function()) {
        // Evaluate arguments
        std::vector<Value> arg_values;
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            arg_values.push_back(arg_value);
        }
        
        // Call the function
        Function* function = callee_value.as_function();
        return function->call(ctx, arg_values);
    }
    
    // Handle console.log specially for Stage 2
    if (callee_->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
        MemberExpression* member = static_cast<MemberExpression*>(callee_.get());
        
        // Check if it's console.log
        if (member->get_object()->get_type() == ASTNode::Type::IDENTIFIER &&
            member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
            
            Identifier* obj = static_cast<Identifier*>(member->get_object());
            Identifier* prop = static_cast<Identifier*>(member->get_property());
            
            if (obj->get_name() == "console" && prop->get_name() == "log") {
                // Evaluate arguments and print them
                std::vector<Value> arg_values;
                for (const auto& arg : arguments_) {
                    Value val = arg->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    arg_values.push_back(val);
                }
                
                // Print arguments separated by spaces
                for (size_t i = 0; i < arg_values.size(); ++i) {
                    if (i > 0) std::cout << " ";
                    std::cout << arg_values[i].to_string();
                }
                std::cout << std::endl;
                
                return Value(); // console.log returns undefined
            }
        }
        
        // Handle general object method calls (obj.method())
        Value object_value = member->get_object()->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        if (object_value.is_object()) {
            Object* obj = object_value.as_object();
            
            // Get the method name
            std::string method_name;
            if (member->is_computed()) {
                // For obj[expr]()
                Value key_value = member->get_property()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                method_name = key_value.to_string();
            } else {
                // For obj.method()
                if (member->get_property()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* prop = static_cast<Identifier*>(member->get_property());
                    method_name = prop->get_name();
                } else {
                    ctx.throw_exception(Value("Invalid method name"));
                    return Value();
                }
            }
            
            // Get the method from the object
            Value method_value = obj->get_property(method_name);
            
            // Check if it's a function
            if (method_value.is_string() && method_value.to_string().find("[Function:") == 0) {
                // Handle array methods specially
                if (obj->is_array()) {
                    return handle_array_method_call(obj, method_name, ctx);
                } else {
                    // For regular object methods, we'll just return a placeholder result
                    // In a full implementation, we'd execute the method body with 'this' binding
                    std::cout << "Calling method: " << method_name << "() on object -> [Method execution not fully implemented yet]" << std::endl;
                    return Value(42.0); // Placeholder return value
                }
            } else {
                ctx.throw_exception(Value("'" + method_name + "' is not a function"));
                return Value();
            }
        } else {
            ctx.throw_exception(Value("Cannot call method on non-object"));
            return Value();
        }
    }
    
    // Handle regular function calls
    if (callee_->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* func_id = static_cast<Identifier*>(callee_.get());
        Value function_value = ctx.get_binding(func_id->get_name());
        
        // Check if it's a function
        if (function_value.is_string() && function_value.to_string().find("[Function:") == 0) {
            // For Stage 4, we'll just return a placeholder result
            // In a full implementation, we'd execute the function body with parameters
            std::cout << "Calling function: " << func_id->get_name() << "() -> [Function execution not fully implemented yet]" << std::endl;
            return Value(42.0); // Placeholder return value
        } else {
            ctx.throw_exception(Value("'" + func_id->get_name() + "' is not a function"));
            return Value();
        }
    }
    
    // For other function calls, we'd need a proper function implementation
    ctx.throw_exception(Value("Function calls not yet implemented"));
    return Value();
}

std::string CallExpression::to_string() const {
    std::ostringstream oss;
    oss << callee_->to_string() << "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << arguments_[i]->to_string();
    }
    oss << ")";
    return oss.str();
}

std::unique_ptr<ASTNode> CallExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    return std::make_unique<CallExpression>(callee_->clone(), std::move(cloned_args), start_, end_);
}

Value CallExpression::handle_array_method_call(Object* array, const std::string& method_name, Context& ctx) {
    if (method_name == "push") {
        // Evaluate all arguments and push them to the array
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->push(arg_value);
        }
        // Return new length
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "pop") {
        // Remove and return the last element
        if (array->get_length() > 0) {
            return array->pop();
        } else {
            return Value(); // undefined
        }
        
    } else if (method_name == "shift") {
        // Remove and return the first element
        if (array->get_length() > 0) {
            return array->shift();
        } else {
            return Value(); // undefined
        }
        
    } else if (method_name == "unshift") {
        // Add elements to the beginning and return new length
        for (const auto& arg : arguments_) {
            Value arg_value = arg->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            array->unshift(arg_value);
        }
        return Value(static_cast<double>(array->get_length()));
        
    } else if (method_name == "join") {
        // Join array elements with separator
        std::string separator = ",";
        if (arguments_.size() > 0) {
            Value sep_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            separator = sep_value.to_string();
        }
        
        std::ostringstream result;
        uint32_t length = array->get_length();
        for (uint32_t i = 0; i < length; ++i) {
            if (i > 0) result << separator;
            Value element = array->get_element(i);
            if (!element.is_undefined() && !element.is_null()) {
                result << element.to_string();
            }
        }
        return Value(result.str());
        
    } else if (method_name == "indexOf") {
        // Find index of element
        if (arguments_.size() > 0) {
            Value search_value = arguments_[0]->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                Value element = array->get_element(i);
                if (element.strict_equals(search_value)) {
                    return Value(static_cast<double>(i));
                }
            }
        }
        return Value(-1.0); // not found
        
    } else {
        std::cout << "Calling array method: " << method_name << "() -> [Method not fully implemented yet]" << std::endl;
        return Value(42.0); // Placeholder for other methods
    }
}

//=============================================================================
// MemberExpression Implementation
//=============================================================================

Value MemberExpression::evaluate(Context& ctx) {
    Value object_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    // For Stage 2, we'll handle basic property access
    // This is a simplified implementation
    if (object_value.is_object()) {
        Object* obj = object_value.as_object();
        if (computed_) {
            Value prop_value = property_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            return obj->get_property(prop_value.to_string());
        } else {
            if (property_->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* prop = static_cast<Identifier*>(property_.get());
                return obj->get_property(prop->get_name());
            }
        }
    }
    
    return Value(); // undefined
}

std::string MemberExpression::to_string() const {
    if (computed_) {
        return object_->to_string() + "[" + property_->to_string() + "]";
    } else {
        return object_->to_string() + "." + property_->to_string();
    }
}

std::unique_ptr<ASTNode> MemberExpression::clone() const {
    return std::make_unique<MemberExpression>(
        object_->clone(), property_->clone(), computed_, start_, end_
    );
}

//=============================================================================
// NewExpression Implementation
//=============================================================================

Value NewExpression::evaluate(Context& ctx) {
    // Evaluate constructor function
    Value constructor_value = constructor_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    if (!constructor_value.is_function()) {
        ctx.throw_exception(Value("TypeError: " + constructor_value.to_string() + " is not a constructor"));
        return Value();
    }
    
    // Evaluate arguments
    std::vector<Value> arg_values;
    for (const auto& arg : arguments_) {
        Value arg_value = arg->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        arg_values.push_back(arg_value);
    }
    
    // Call constructor function
    Function* constructor_fn = constructor_value.as_function();
    return constructor_fn->construct(ctx, arg_values);
}

std::string NewExpression::to_string() const {
    std::string result = "new " + constructor_->to_string() + "(";
    for (size_t i = 0; i < arguments_.size(); ++i) {
        if (i > 0) result += ", ";
        result += arguments_[i]->to_string();
    }
    result += ")";
    return result;
}

std::unique_ptr<ASTNode> NewExpression::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_args;
    for (const auto& arg : arguments_) {
        cloned_args.push_back(arg->clone());
    }
    return std::make_unique<NewExpression>(
        constructor_->clone(), std::move(cloned_args), start_, end_
    );
}

//=============================================================================
// ExpressionStatement Implementation
//=============================================================================

Value ExpressionStatement::evaluate(Context& ctx) {
    return expression_->evaluate(ctx);
}

std::string ExpressionStatement::to_string() const {
    return expression_->to_string() + ";";
}

std::unique_ptr<ASTNode> ExpressionStatement::clone() const {
    return std::make_unique<ExpressionStatement>(expression_->clone(), start_, end_);
}

//=============================================================================
// Program Implementation
//=============================================================================

Value Program::evaluate(Context& ctx) {
    Value last_value;
    
    for (const auto& statement : statements_) {
        last_value = statement->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }
    }
    
    return last_value;
}

std::string Program::to_string() const {
    std::ostringstream oss;
    for (const auto& statement : statements_) {
        oss << statement->to_string() << "\n";
    }
    return oss.str();
}

std::unique_ptr<ASTNode> Program::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<Program>(std::move(cloned_statements), start_, end_);
}

//=============================================================================
// VariableDeclarator Implementation
//=============================================================================

Value VariableDeclarator::evaluate(Context& ctx) {
    // Variable declarators don't get evaluated directly - they're evaluated by VariableDeclaration
    (void)ctx;
    return Value();
}

std::string VariableDeclarator::to_string() const {
    std::string result = id_->get_name();
    if (init_) {
        result += " = " + init_->to_string();
    }
    return result;
}

std::unique_ptr<ASTNode> VariableDeclarator::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    return std::make_unique<VariableDeclarator>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_init), kind_, start_, end_
    );
}

std::string VariableDeclarator::kind_to_string(Kind kind) {
    switch (kind) {
        case Kind::VAR: return "var";
        case Kind::LET: return "let";
        case Kind::CONST: return "const";
        default: return "var";
    }
}

//=============================================================================
// VariableDeclaration Implementation
//=============================================================================

Value VariableDeclaration::evaluate(Context& ctx) {
    for (const auto& declarator : declarations_) {
        const std::string& name = declarator->get_id()->get_name();
        
        // Evaluate initializer if present
        Value init_value;
        if (declarator->get_init()) {
            init_value = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        } else {
            init_value = Value(); // undefined
        }
        
        // Create binding based on declaration kind
        bool mutable_binding = (declarator->get_kind() != VariableDeclarator::Kind::CONST);
        
        if (!ctx.create_binding(name, init_value, mutable_binding)) {
            ctx.throw_exception(Value("Variable '" + name + "' already declared"));
            return Value();
        }
    }
    
    return Value(); // Variable declarations return undefined
}

std::string VariableDeclaration::to_string() const {
    std::ostringstream oss;
    oss << VariableDeclarator::kind_to_string(kind_) << " ";
    for (size_t i = 0; i < declarations_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << declarations_[i]->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> VariableDeclaration::clone() const {
    std::vector<std::unique_ptr<VariableDeclarator>> cloned_declarations;
    for (const auto& decl : declarations_) {
        cloned_declarations.push_back(
            std::unique_ptr<VariableDeclarator>(static_cast<VariableDeclarator*>(decl->clone().release()))
        );
    }
    return std::make_unique<VariableDeclaration>(std::move(cloned_declarations), kind_, start_, end_);
}

//=============================================================================
// BlockStatement Implementation
//=============================================================================

Value BlockStatement::evaluate(Context& ctx) {
    Value last_value;
    
    // Create new block scope for let/const declarations
    // For now, we'll use the same context (simplified scope handling)
    
    for (const auto& statement : statements_) {
        last_value = statement->evaluate(ctx);
        if (ctx.has_exception()) {
            return Value();
        }
    }
    
    return last_value;
}

std::string BlockStatement::to_string() const {
    std::ostringstream oss;
    oss << "{\n";
    for (const auto& statement : statements_) {
        oss << "  " << statement->to_string() << "\n";
    }
    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> BlockStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_statements;
    for (const auto& statement : statements_) {
        cloned_statements.push_back(statement->clone());
    }
    return std::make_unique<BlockStatement>(std::move(cloned_statements), start_, end_);
}

//=============================================================================
// IfStatement Implementation
//=============================================================================

Value IfStatement::evaluate(Context& ctx) {
    // Evaluate test condition
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    // Convert to boolean and choose branch
    if (test_value.to_boolean()) {
        return consequent_->evaluate(ctx);
    } else if (alternate_) {
        return alternate_->evaluate(ctx);
    }
    
    return Value(); // undefined
}

std::string IfStatement::to_string() const {
    std::ostringstream oss;
    oss << "if (" << test_->to_string() << ") " << consequent_->to_string();
    if (alternate_) {
        oss << " else " << alternate_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> IfStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_alternate = alternate_ ? alternate_->clone() : nullptr;
    return std::make_unique<IfStatement>(
        test_->clone(), consequent_->clone(), std::move(cloned_alternate), start_, end_
    );
}

//=============================================================================
// ForStatement Implementation
//=============================================================================

Value ForStatement::evaluate(Context& ctx) {
    // Execute initialization
    if (init_) {
        init_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    
    while (true) {
        // Evaluate test condition
        if (test_) {
            Value test_value = test_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            
            if (!test_value.to_boolean()) {
                break; // Exit loop if condition is false
            }
        }
        
        // Execute body
        Value body_result = body_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        // Handle break/continue (for now, just continue)
        // TODO: Implement proper break/continue handling
        
        // Execute update expression
        if (update_) {
            update_->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        }
    }
    
    return Value(); // undefined
}

std::string ForStatement::to_string() const {
    std::ostringstream oss;
    oss << "for (";
    if (init_) oss << init_->to_string();
    oss << "; ";
    if (test_) oss << test_->to_string();
    oss << "; ";
    if (update_) oss << update_->to_string();
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> ForStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_init = init_ ? init_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_test = test_ ? test_->clone() : nullptr;
    std::unique_ptr<ASTNode> cloned_update = update_ ? update_->clone() : nullptr;
    return std::make_unique<ForStatement>(
        std::move(cloned_init), std::move(cloned_test), 
        std::move(cloned_update), body_->clone(), start_, end_
    );
}

//=============================================================================
// WhileStatement Implementation
//=============================================================================

Value WhileStatement::evaluate(Context& ctx) {
    while (true) {
        // Evaluate test condition
        Value test_value = test_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        if (!test_value.to_boolean()) {
            break; // Exit loop if condition is false
        }
        
        // Execute body
        Value body_result = body_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        // Handle break/continue (for now, just continue)
        // TODO: Implement proper break/continue handling
    }
    
    return Value(); // undefined
}

std::string WhileStatement::to_string() const {
    return "while (" + test_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WhileStatement::clone() const {
    return std::make_unique<WhileStatement>(
        test_->clone(), body_->clone(), start_, end_
    );
}

//=============================================================================
// FunctionDeclaration Implementation
//=============================================================================

Value FunctionDeclaration::evaluate(Context& ctx) {
    // Create a function object with the parsed body and parameters
    const std::string& function_name = id_->get_name();
    
    // Extract parameter names
    std::vector<std::string> param_names;
    for (const auto& param : params_) {
        param_names.push_back(param->get_name());
    }
    
    // Create Function object
    auto function_obj = ObjectFactory::create_js_function(
        function_name, 
        param_names, 
        body_->clone(),  // Clone the AST body
        &ctx             // Current context as closure
    );
    
    // Wrap in Value
    Value function_value = ValueFactory::create_function(std::move(function_obj));
    
    // Create binding in current context
    if (!ctx.create_binding(function_name, function_value, true)) {
        ctx.throw_exception(Value("Function '" + function_name + "' already declared"));
        return Value();
    }
    
    return Value(); // Function declarations return undefined
}

std::string FunctionDeclaration::to_string() const {
    std::ostringstream oss;
    oss << "function " << id_->get_name() << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionDeclaration::clone() const {
    std::vector<std::unique_ptr<Identifier>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(param->clone().release()))
        );
    }
    
    return std::make_unique<FunctionDeclaration>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}

//=============================================================================
// FunctionExpression Implementation
//=============================================================================

Value FunctionExpression::evaluate(Context& ctx) {
    // Create function value for expression
    std::string name = is_named() ? id_->get_name() : "<anonymous>";
    Value function_value = ValueFactory::function_placeholder(name);
    
    return function_value;
}

std::string FunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "function";
    if (is_named()) {
        oss << " " << id_->get_name();
    }
    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionExpression::clone() const {
    std::vector<std::unique_ptr<Identifier>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(param->clone().release()))
        );
    }
    
    std::unique_ptr<Identifier> cloned_id = nullptr;
    if (is_named()) {
        cloned_id = std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release()));
    }
    
    return std::make_unique<FunctionExpression>(
        std::move(cloned_id),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}

//=============================================================================
// ReturnStatement Implementation
//=============================================================================

Value ReturnStatement::evaluate(Context& ctx) {
    Value return_value;
    
    if (has_argument()) {
        return_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    } else {
        return_value = Value(); // undefined
    }
    
    // Set return value in context
    ctx.set_return_value(return_value);
    return return_value;
}

std::string ReturnStatement::to_string() const {
    std::ostringstream oss;
    oss << "return";
    if (has_argument()) {
        oss << " " << argument_->to_string();
    }
    oss << ";";
    return oss.str();
}

std::unique_ptr<ASTNode> ReturnStatement::clone() const {
    std::unique_ptr<ASTNode> cloned_argument = nullptr;
    if (has_argument()) {
        cloned_argument = argument_->clone();
    }
    
    return std::make_unique<ReturnStatement>(std::move(cloned_argument), start_, end_);
}

//=============================================================================
// ObjectLiteral Implementation
//=============================================================================

Value ObjectLiteral::evaluate(Context& ctx) {
    // Create a new object
    auto object = ObjectFactory::create_object();
    
    // Add all properties to the object
    for (const auto& prop : properties_) {
        std::string key;
        
        // Evaluate the key
        if (prop->computed) {
            // For computed properties [expr]: value, evaluate the expression
            Value key_value = prop->key->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            key = key_value.to_string();
        } else {
            // For regular properties, the key should be an identifier
            if (prop->key->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(prop->key.get());
                key = id->get_name();
            } else {
                ctx.throw_exception(Value("Invalid property key in object literal"));
                return Value();
            }
        }
        
        // Evaluate the value
        Value value = prop->value->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        // Set the property on the object
        object->set_property(key, value);
    }
    
    return Value(object.release());
}

std::string ObjectLiteral::to_string() const {
    std::ostringstream oss;
    oss << "{";
    
    for (size_t i = 0; i < properties_.size(); ++i) {
        if (i > 0) oss << ", ";
        
        if (properties_[i]->computed) {
            oss << "[" << properties_[i]->key->to_string() << "]";
        } else {
            oss << properties_[i]->key->to_string();
        }
        
        oss << ": " << properties_[i]->value->to_string();
    }
    
    oss << "}";
    return oss.str();
}

std::unique_ptr<ASTNode> ObjectLiteral::clone() const {
    std::vector<std::unique_ptr<Property>> cloned_properties;
    
    for (const auto& prop : properties_) {
        auto cloned_prop = std::make_unique<Property>(
            prop->key->clone(),
            prop->value->clone(),
            prop->computed,
            prop->method
        );
        cloned_properties.push_back(std::move(cloned_prop));
    }
    
    return std::make_unique<ObjectLiteral>(std::move(cloned_properties), start_, end_);
}

//=============================================================================
// ArrayLiteral Implementation
//=============================================================================

Value ArrayLiteral::evaluate(Context& ctx) {
    // Create a new array object
    auto array = ObjectFactory::create_array(elements_.size());
    
    // Add all elements to the array
    for (size_t i = 0; i < elements_.size(); ++i) {
        Value element_value = elements_[i]->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        
        // Set the element at index i
        array->set_element(static_cast<uint32_t>(i), element_value);
    }
    
    // Add array methods as function placeholders
    array->set_property("push", ValueFactory::function_placeholder("push"));
    array->set_property("pop", ValueFactory::function_placeholder("pop"));
    array->set_property("shift", ValueFactory::function_placeholder("shift"));
    array->set_property("unshift", ValueFactory::function_placeholder("unshift"));
    array->set_property("slice", ValueFactory::function_placeholder("slice"));
    array->set_property("splice", ValueFactory::function_placeholder("splice"));
    array->set_property("indexOf", ValueFactory::function_placeholder("indexOf"));
    array->set_property("join", ValueFactory::function_placeholder("join"));
    
    return Value(array.release());
}

std::string ArrayLiteral::to_string() const {
    std::ostringstream oss;
    oss << "[";
    
    for (size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << elements_[i]->to_string();
    }
    
    oss << "]";
    return oss.str();
}

std::unique_ptr<ASTNode> ArrayLiteral::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_elements;
    
    for (const auto& element : elements_) {
        cloned_elements.push_back(element->clone());
    }
    
    return std::make_unique<ArrayLiteral>(std::move(cloned_elements), start_, end_);
}

//=============================================================================
// Stage 9: Error Handling & Advanced Control Flow Implementation
//=============================================================================

Value TryStatement::evaluate(Context& ctx) {
    Value result;
    bool exception_caught = false;
    
    // Execute try block
    try {
        result = try_block_->evaluate(ctx);
        
        // Check if an exception was thrown during evaluation
        if (ctx.has_exception()) {
            // If we have a catch clause, handle the exception
            if (catch_clause_) {
                Value exception = ctx.get_exception();
                ctx.clear_exception();
                
                // Create new scope for catch block with exception parameter
                auto catch_context = ContextFactory::create_eval_context(ctx.get_engine(), &ctx);
                CatchClause* catch_node = static_cast<CatchClause*>(catch_clause_.get());
                catch_context->create_binding(catch_node->get_parameter_name(), exception);
                
                // Execute catch block
                result = catch_node->get_body()->evaluate(*catch_context);
                exception_caught = true;
            }
        }
    } catch (const std::exception& e) {
        // Handle C++ exceptions as JavaScript exceptions
        if (catch_clause_) {
            Value exception = Value(e.what());
            ctx.clear_exception();
            
            auto catch_context = ContextFactory::create_eval_context(ctx.get_engine(), &ctx);
            CatchClause* catch_node = static_cast<CatchClause*>(catch_clause_.get());
            catch_context->create_binding(catch_node->get_parameter_name(), exception);
            
            result = catch_node->get_body()->evaluate(*catch_context);
            exception_caught = true;
        } else {
            // Re-throw if no catch clause
            ctx.throw_exception(Value(e.what()));
        }
    }
    
    // Execute finally block if present
    if (finally_block_) {
        finally_block_->evaluate(ctx);
        // Finally block doesn't change the result, but can throw new exceptions
    }
    
    return result;
}

std::string TryStatement::to_string() const {
    std::string result = "try " + try_block_->to_string();
    
    if (catch_clause_) {
        result += " " + catch_clause_->to_string();
    }
    
    if (finally_block_) {
        result += " finally " + finally_block_->to_string();
    }
    
    return result;
}

std::unique_ptr<ASTNode> TryStatement::clone() const {
    auto cloned_try = try_block_->clone();
    auto cloned_catch = catch_clause_ ? catch_clause_->clone() : nullptr;
    auto cloned_finally = finally_block_ ? finally_block_->clone() : nullptr;
    
    return std::make_unique<TryStatement>(
        std::move(cloned_try), 
        std::move(cloned_catch), 
        std::move(cloned_finally), 
        start_, end_
    );
}

Value CatchClause::evaluate(Context& ctx) {
    // This is called from TryStatement, the parameter binding is handled there
    return body_->evaluate(ctx);
}

std::string CatchClause::to_string() const {
    return "catch (" + parameter_name_ + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> CatchClause::clone() const {
    return std::make_unique<CatchClause>(parameter_name_, body_->clone(), start_, end_);
}

Value ThrowStatement::evaluate(Context& ctx) {
    Value exception_value = expression_->evaluate(ctx);
    if (ctx.has_exception()) return Value(); // Already has exception
    
    // Throw the exception
    ctx.throw_exception(exception_value);
    return Value(); // This shouldn't be reached due to exception
}

std::string ThrowStatement::to_string() const {
    return "throw " + expression_->to_string();
}

std::unique_ptr<ASTNode> ThrowStatement::clone() const {
    return std::make_unique<ThrowStatement>(expression_->clone(), start_, end_);
}

Value SwitchStatement::evaluate(Context& ctx) {
    // Evaluate the discriminant (the value to switch on)
    Value discriminant_value = discriminant_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    bool found_match = false;
    bool fall_through = false;
    Value result;
    
    // Look for matching case or default
    for (const auto& case_node : cases_) {
        CaseClause* case_clause = static_cast<CaseClause*>(case_node.get());
        
        // Check if this case matches or if we're falling through
        bool should_execute = fall_through;
        
        if (!fall_through) {
            if (case_clause->is_default()) {
                // Default case - execute if no previous match
                should_execute = !found_match;
            } else {
                // Regular case - check for equality
                Value test_value = case_clause->get_test()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                // Use strict equality for switch cases
                should_execute = discriminant_value.strict_equals(test_value);
            }
        }
        
        if (should_execute) {
            found_match = true;
            fall_through = true;
            
            // Execute all statements in this case
            for (const auto& stmt : case_clause->get_consequent()) {
                result = stmt->evaluate(ctx);
                if (ctx.has_exception()) return Value();
                
                // Check for break statement (we'd need to implement this)
                // For now, we'll implement basic switch without break
            }
        }
    }
    
    return found_match ? result : Value();
}

std::string SwitchStatement::to_string() const {
    std::string result = "switch (" + discriminant_->to_string() + ") {\n";
    
    for (const auto& case_node : cases_) {
        result += "  " + case_node->to_string() + "\n";
    }
    
    result += "}";
    return result;
}

std::unique_ptr<ASTNode> SwitchStatement::clone() const {
    std::vector<std::unique_ptr<ASTNode>> cloned_cases;
    for (const auto& case_node : cases_) {
        cloned_cases.push_back(case_node->clone());
    }
    
    return std::make_unique<SwitchStatement>(
        discriminant_->clone(),
        std::move(cloned_cases),
        start_, end_
    );
}

Value CaseClause::evaluate(Context& ctx) {
    // Execute all consequent statements
    Value result;
    for (const auto& stmt : consequent_) {
        result = stmt->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    return result;
}

std::string CaseClause::to_string() const {
    std::string result;
    
    if (is_default()) {
        result = "default:";
    } else {
        result = "case " + test_->to_string() + ":";
    }
    
    for (const auto& stmt : consequent_) {
        result += " " + stmt->to_string() + ";";
    }
    
    return result;
}

std::unique_ptr<ASTNode> CaseClause::clone() const {
    auto cloned_test = test_ ? test_->clone() : nullptr;
    
    std::vector<std::unique_ptr<ASTNode>> cloned_consequent;
    for (const auto& stmt : consequent_) {
        cloned_consequent.push_back(stmt->clone());
    }
    
    return std::make_unique<CaseClause>(
        std::move(cloned_test),
        std::move(cloned_consequent),
        start_, end_
    );
}

//=============================================================================
// Stage 10: Import/Export AST evaluation
//=============================================================================

// ImportSpecifier evaluation
Value ImportSpecifier::evaluate(Context& ctx) {
    // Import specifiers are handled by ImportStatement
    return Value(); // undefined
}

std::string ImportSpecifier::to_string() const {
    if (imported_name_ != local_name_) {
        return imported_name_ + " as " + local_name_;
    }
    return imported_name_;
}

std::unique_ptr<ASTNode> ImportSpecifier::clone() const {
    return std::make_unique<ImportSpecifier>(imported_name_, local_name_, start_, end_);
}

// ImportStatement evaluation
Value ImportStatement::evaluate(Context& ctx) {
    // Get the module loader from the engine
    auto* engine = ctx.get_engine();
    if (!engine) {
        throw std::runtime_error("No engine available for module loading");
    }
    
    auto* module_loader = engine->get_module_loader();
    if (!module_loader) {
        throw std::runtime_error("No module loader available");
    }
    
    if (is_namespace_import_) {
        // import * as name from "module"
        Value namespace_obj = module_loader->import_namespace_from_module(module_source_);
        ctx.create_binding(namespace_alias_, namespace_obj);
    } else if (is_default_import_) {
        // import name from "module"
        Value default_value = module_loader->import_default_from_module(module_source_);
        ctx.create_binding(default_alias_, default_value);
    } else {
        // import { name1, name2 as alias } from "module"
        for (const auto& specifier : specifiers_) {
            Value imported_value = module_loader->import_from_module(
                module_source_, 
                specifier->get_imported_name()
            );
            ctx.create_binding(specifier->get_local_name(), imported_value);
        }
    }
    
    return Value(); // undefined
}

std::string ImportStatement::to_string() const {
    std::string result = "import ";
    
    if (is_namespace_import_) {
        result += "* as " + namespace_alias_;
    } else if (is_default_import_) {
        result += default_alias_;
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
    }
    
    result += " from \"" + module_source_ + "\"";
    return result;
}

std::unique_ptr<ASTNode> ImportStatement::clone() const {
    if (is_namespace_import_) {
        return std::make_unique<ImportStatement>(namespace_alias_, module_source_, start_, end_);
    } else if (is_default_import_) {
        return std::make_unique<ImportStatement>(default_alias_, module_source_, true, start_, end_);
    } else {
        std::vector<std::unique_ptr<ImportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ImportSpecifier>(
                    spec->get_imported_name(),
                    spec->get_local_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        return std::make_unique<ImportStatement>(std::move(cloned_specifiers), module_source_, start_, end_);
    }
}

// ExportSpecifier evaluation
Value ExportSpecifier::evaluate(Context& ctx) {
    // Export specifiers are handled by ExportStatement
    return Value(); // undefined
}

std::string ExportSpecifier::to_string() const {
    if (local_name_ != exported_name_) {
        return local_name_ + " as " + exported_name_;
    }
    return local_name_;
}

std::unique_ptr<ASTNode> ExportSpecifier::clone() const {
    return std::make_unique<ExportSpecifier>(local_name_, exported_name_, start_, end_);
}

// ExportStatement evaluation
Value ExportStatement::evaluate(Context& ctx) {
    // Get the exports object from module context
    Value exports_value = ctx.get_binding("exports");
    if (!exports_value.is_object()) {
        // Create exports object if it doesn't exist
        auto exports_obj = std::make_shared<Object>();
        exports_value = Value(exports_obj.get());
        ctx.create_binding("exports", exports_value);
    }
    
    auto exports_obj = exports_value.as_object();
    
    if (is_default_export_) {
        // export default expression
        Value default_value = default_export_->evaluate(ctx);
        exports_obj->set_property("default", default_value);
    } else if (is_declaration_export_) {
        // export function name() {} or export var name = value
        Value declaration_result = declaration_->evaluate(ctx);
        
        // For function/variable declarations, extract the name and add to exports
        if (declaration_->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            auto* func_decl = static_cast<FunctionDeclaration*>(declaration_.get());
            exports_obj->set_property(func_decl->get_id()->get_name(), declaration_result);
        } else if (declaration_->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
            auto* var_decl = static_cast<VariableDeclaration*>(declaration_.get());
            for (const auto& declarator : var_decl->get_declarations()) {
                auto* var_declarator = static_cast<VariableDeclarator*>(declarator.get());
                Value var_value = ctx.get_binding(var_declarator->get_id()->get_name());
                exports_obj->set_property(var_declarator->get_id()->get_name(), var_value);
            }
        }
    } else if (is_re_export_) {
        // export { name } from "module"
        auto* engine = ctx.get_engine();
        auto* module_loader = engine->get_module_loader();
        
        for (const auto& specifier : specifiers_) {
            Value imported_value = module_loader->import_from_module(
                source_module_,
                specifier->get_local_name()
            );
            exports_obj->set_property(specifier->get_exported_name(), imported_value);
        }
    } else {
        // export { name1, name2 as alias }
        for (const auto& specifier : specifiers_) {
            Value local_value = ctx.get_binding(specifier->get_local_name());
            exports_obj->set_property(specifier->get_exported_name(), local_value);
        }
    }
    
    return Value(); // undefined
}

std::string ExportStatement::to_string() const {
    std::string result = "export ";
    
    if (is_default_export_) {
        result += "default " + default_export_->to_string();
    } else if (is_declaration_export_) {
        result += declaration_->to_string();
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
        
        if (is_re_export_) {
            result += " from \"" + source_module_ + "\"";
        }
    }
    
    return result;
}

std::unique_ptr<ASTNode> ExportStatement::clone() const {
    if (is_default_export_) {
        return std::make_unique<ExportStatement>(default_export_->clone(), true, start_, end_);
    } else if (is_declaration_export_) {
        return std::make_unique<ExportStatement>(declaration_->clone(), start_, end_);
    } else {
        std::vector<std::unique_ptr<ExportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ExportSpecifier>(
                    spec->get_local_name(),
                    spec->get_exported_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        
        if (is_re_export_) {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), source_module_, start_, end_);
        } else {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), start_, end_);
        }
    }
}

} // namespace Quanta