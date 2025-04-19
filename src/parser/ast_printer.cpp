#include "ast_printer.h"
#include <sstream>
#include <variant>

namespace quanta {

void ASTPrinter::print(const std::shared_ptr<Program>& program) {
    program->accept(*this);
}

void ASTPrinter::indent() {
    for (int i = 0; i < indent_level_; i++) {
        std::cout << "  ";
    }
}

void ASTPrinter::increase_indent() {
    indent_level_++;
}

void ASTPrinter::decrease_indent() {
    if (indent_level_ > 0) {
        indent_level_--;
    }
}

void ASTPrinter::println(const std::string& text) {
    indent();
    std::cout << text << std::endl;
}

// Statement visitor methods

void ASTPrinter::visit(Program& stmt) {
    println("Program");
    increase_indent();
    
    println("Body:");
    increase_indent();
    for (const auto& statement : stmt.body) {
        if (statement) {
            statement->accept(*this);
        } else {
            println("<null statement>");
        }
    }
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(BlockStatement& stmt) {
    println("BlockStatement");
    increase_indent();
    
    for (const auto& statement : stmt.body) {
        if (statement) {
            statement->accept(*this);
        } else {
            println("<null statement>");
        }
    }
    
    decrease_indent();
}

void ASTPrinter::visit(ExpressionStatement& stmt) {
    println("ExpressionStatement");
    increase_indent();
    
    if (stmt.expression) {
        stmt.expression->accept(*this);
    } else {
        println("<null expression>");
    }
    
    decrease_indent();
}

void ASTPrinter::visit(VariableDeclaration& stmt) {
    std::string kind;
    switch (stmt.kind) {
        case VariableDeclaration::Kind::Var: kind = "var"; break;
        case VariableDeclaration::Kind::Let: kind = "let"; break;
        case VariableDeclaration::Kind::Const: kind = "const"; break;
    }
    
    println("VariableDeclaration (" + kind + ")");
    increase_indent();
    
    for (const auto& declarator : stmt.declarations) {
        println("VariableDeclarator");
        increase_indent();
        
        println("Identifier: " + declarator->id->name);
        
        if (declarator->init) {
            println("Initializer:");
            increase_indent();
            declarator->init->accept(*this);
            decrease_indent();
        } else {
            println("Initializer: <none>");
        }
        
        decrease_indent();
    }
    
    decrease_indent();
}

void ASTPrinter::visit(FunctionDeclaration& stmt) {
    println("FunctionDeclaration: " + stmt.id->name);
    increase_indent();
    
    println("Parameters:");
    increase_indent();
    for (const auto& param : stmt.params) {
        println(param->name);
    }
    decrease_indent();
    
    println("Body:");
    increase_indent();
    stmt.body->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(ReturnStatement& stmt) {
    println("ReturnStatement");
    increase_indent();
    
    if (stmt.argument) {
        stmt.argument->accept(*this);
    } else {
        println("<no return value>");
    }
    
    decrease_indent();
}

void ASTPrinter::visit(IfStatement& stmt) {
    println("IfStatement");
    increase_indent();
    
    println("Test:");
    increase_indent();
    stmt.test->accept(*this);
    decrease_indent();
    
    println("Consequent:");
    increase_indent();
    stmt.consequent->accept(*this);
    decrease_indent();
    
    if (stmt.alternate) {
        println("Alternate:");
        increase_indent();
        stmt.alternate->accept(*this);
        decrease_indent();
    }
    
    decrease_indent();
}

void ASTPrinter::visit(WhileStatement& stmt) {
    println("WhileStatement");
    increase_indent();
    
    println("Test:");
    increase_indent();
    stmt.test->accept(*this);
    decrease_indent();
    
    println("Body:");
    increase_indent();
    stmt.body->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(ForStatement& stmt) {
    println("ForStatement");
    increase_indent();
    
    println("Init:");
    increase_indent();
    if (std::holds_alternative<std::monostate>(stmt.init)) {
        println("<none>");
    } else if (std::holds_alternative<std::shared_ptr<VariableDeclaration>>(stmt.init)) {
        std::get<std::shared_ptr<VariableDeclaration>>(stmt.init)->accept(*this);
    } else if (std::holds_alternative<std::shared_ptr<Expression>>(stmt.init)) {
        std::get<std::shared_ptr<Expression>>(stmt.init)->accept(*this);
    }
    decrease_indent();
    
    println("Test:");
    increase_indent();
    if (stmt.test) {
        stmt.test->accept(*this);
    } else {
        println("<none>");
    }
    decrease_indent();
    
    println("Update:");
    increase_indent();
    if (stmt.update) {
        stmt.update->accept(*this);
    } else {
        println("<none>");
    }
    decrease_indent();
    
    println("Body:");
    increase_indent();
    stmt.body->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

// Expression visitor methods

void ASTPrinter::visit(Identifier& expr) {
    println("Identifier: " + expr.name);
}

void ASTPrinter::visit(Literal& expr) {
    std::string value;
    
    if (std::holds_alternative<std::monostate>(expr.value)) {
        value = "undefined";
    } else if (std::holds_alternative<std::string>(expr.value)) {
        value = "\"" + std::get<std::string>(expr.value) + "\"";
    } else if (std::holds_alternative<double>(expr.value)) {
        value = std::to_string(std::get<double>(expr.value));
    } else if (std::holds_alternative<bool>(expr.value)) {
        value = std::get<bool>(expr.value) ? "true" : "false";
    } else if (std::holds_alternative<std::nullptr_t>(expr.value)) {
        value = "null";
    }
    
    println("Literal: " + value);
}

void ASTPrinter::visit(BinaryExpression& expr) {
    println("BinaryExpression: " + expr.operator_);
    increase_indent();
    
    println("Left:");
    increase_indent();
    expr.left->accept(*this);
    decrease_indent();
    
    println("Right:");
    increase_indent();
    expr.right->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(LogicalExpression& expr) {
    println("LogicalExpression: " + expr.operator_);
    increase_indent();
    
    println("Left:");
    increase_indent();
    expr.left->accept(*this);
    decrease_indent();
    
    println("Right:");
    increase_indent();
    expr.right->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(UnaryExpression& expr) {
    println("UnaryExpression: " + expr.operator_ + (expr.prefix ? " (prefix)" : " (postfix)"));
    increase_indent();
    
    expr.argument->accept(*this);
    
    decrease_indent();
}

void ASTPrinter::visit(AssignmentExpression& expr) {
    println("AssignmentExpression: " + expr.operator_);
    increase_indent();
    
    println("Left:");
    increase_indent();
    expr.left->accept(*this);
    decrease_indent();
    
    println("Right:");
    increase_indent();
    expr.right->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(CallExpression& expr) {
    println("CallExpression");
    increase_indent();
    
    println("Callee:");
    increase_indent();
    expr.callee->accept(*this);
    decrease_indent();
    
    println("Arguments:");
    increase_indent();
    for (const auto& arg : expr.arguments) {
        arg->accept(*this);
    }
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(MemberExpression& expr) {
    println("MemberExpression: " + std::string(expr.computed ? "computed" : "static"));
    increase_indent();
    
    println("Object:");
    increase_indent();
    expr.object->accept(*this);
    decrease_indent();
    
    println("Property:");
    increase_indent();
    expr.property->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(FunctionExpression& expr) {
    println("FunctionExpression" + (expr.id ? ": " + expr.id->name : " (anonymous)"));
    increase_indent();
    
    println("Parameters:");
    increase_indent();
    for (const auto& param : expr.params) {
        println(param->name);
    }
    decrease_indent();
    
    println("Body:");
    increase_indent();
    expr.body->accept(*this);
    decrease_indent();
    
    decrease_indent();
}

void ASTPrinter::visit(ObjectExpression& expr) {
    println("ObjectExpression");
    increase_indent();
    
    for (const auto& prop : expr.properties) {
        std::string kind;
        switch (prop->kind) {
            case Property::Kind::Init: kind = "init"; break;
            case Property::Kind::Get: kind = "get"; break;
            case Property::Kind::Set: kind = "set"; break;
        }
        
        println("Property (" + kind + "):");
        increase_indent();
        
        println("Key:");
        increase_indent();
        prop->key->accept(*this);
        decrease_indent();
        
        println("Value:");
        increase_indent();
        prop->value->accept(*this);
        decrease_indent();
        
        decrease_indent();
    }
    
    decrease_indent();
}

void ASTPrinter::visit(ArrayExpression& expr) {
    println("ArrayExpression");
    increase_indent();
    
    for (size_t i = 0; i < expr.elements.size(); i++) {
        println("Element " + std::to_string(i) + ":");
        increase_indent();
        
        if (expr.elements[i]) {
            expr.elements[i]->accept(*this);
        } else {
            println("<empty>");
        }
        
        decrease_indent();
    }
    
    decrease_indent();
}

void ASTPrinter::visit(ThisExpression& expr) {
    println("ThisExpression");
}

} // namespace quanta 