#include "../include/AST.h"
#include "../../core/include/Context.h"
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
    if (operator_ == Operator::ASSIGN) {
        Value right_value = right_->evaluate(ctx);
        
        // For now, only support simple identifier assignment
        if (left_->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            ctx.set_binding(id->get_name(), right_value);
            return right_value;
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
    Value operand_value = operand_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    switch (operator_) {
        case Operator::PLUS:
            return operand_value.unary_plus();
        case Operator::MINUS:
            return operand_value.unary_minus();
        case Operator::LOGICAL_NOT:
            return operand_value.logical_not();
        case Operator::BITWISE_NOT:
            return operand_value.bitwise_not();
        case Operator::TYPEOF:
            return operand_value.typeof_op();
        case Operator::VOID:
            return Value(); // void always returns undefined
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

} // namespace Quanta