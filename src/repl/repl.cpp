#include "repl.h"
#include "ast.h"
#include "parser.h"
#include "interpreter.h"
#include "lexer.h"
#include <iostream>
#include <string>

namespace quanta {

REPL::REPL(std::shared_ptr<Interpreter> interpreter)
    : interpreter_(interpreter) {
}

void REPL::start() {
    std::cout << "Quanta JavaScript Engine v0.0.1" << std::endl;
    std::cout << "Type .help for assistance" << std::endl;
    
    std::string line;
    bool running = true;
    
    while (running) {
        std::cout << "> ";
        
        if (!std::getline(std::cin, line)) {
            // Handle EOF (Ctrl+D)
            std::cout << std::endl << "Exiting..." << std::endl;
            break;
        }
        
        // Check for special commands
        if (line == ".help") {
            displayHelp();
            continue;
        } else if (line == ".exit") {
            std::cout << "Exiting..." << std::endl;
            break;
        }
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        try {
            // Parse the line to check if it's a console.log call (don't show result for those)
            bool shouldPrintResult = true;
            
            try {
                Lexer lexer(line);
                std::vector<Token> tokens = lexer.scan_tokens();
                Parser parser(tokens);
                std::shared_ptr<Program> program = parser.parse();
                
                // Check if last statement is a console.log call
                if (!program->body.empty()) {
                    auto lastStmt = program->body.back();
                    if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatement>(lastStmt)) {
                        if (auto callExpr = std::dynamic_pointer_cast<CallExpression>(exprStmt->expression)) {
                            if (auto memberExpr = std::dynamic_pointer_cast<MemberExpression>(callExpr->callee)) {
                                if (auto objId = std::dynamic_pointer_cast<Identifier>(memberExpr->object)) {
                                    if (auto propId = std::dynamic_pointer_cast<Identifier>(memberExpr->property)) {
                                        if (objId->name == "console" && propId->name == "log") {
                                            shouldPrintResult = false;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {
                // Ignore any parsing errors here, we'll handle them in the evaluate call
            }
            
            // Execute the line using the interpreter
            JSValue result = interpreter_->evaluate(line);
            
            // Print the result if it's not undefined and not a console.log statement
            if (shouldPrintResult && !std::holds_alternative<std::monostate>(result)) {
                std::cout << interpreter_->value_to_string(result) << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
}

void REPL::displayHelp() {
    std::cout << "Commands:\n"
              << "  .help    Display this help message\n"
              << "  .exit    Exit the REPL\n"
              << "  Ctrl+D   Exit the REPL\n";
}

} // namespace quanta 