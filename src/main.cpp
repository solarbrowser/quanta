#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "lexer.h"
#include "parser.h"
#include "ast_printer.h"
#include "interpreter.h"

using namespace quanta;

/**
 * Read a file into a string
 * @param filename The file path
 * @return The file contents
 */
std::string read_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        exit(EXIT_FAILURE);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * Check if a line is the start of a multi-line statement
 */
bool is_multi_line_start(const std::string& line) {
    return line.find("function") != std::string::npos ||
           line.find("if") != std::string::npos ||
           line.find("for") != std::string::npos ||
           line.find("while") != std::string::npos ||
           line.find("{") != std::string::npos;
}

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    std::cout << "Quanta JavaScript Engine v0.0.1" << std::endl;
    
    std::string source;
    
    if (argc > 1) {
        // Read from file
        source = read_file(argv[1]);
        
        try {
            // Stage 1: Lexical analysis
            Lexer lexer(source);
            std::vector<Token> tokens = lexer.scan_tokens();
            
            // Print tokens
            std::cout << "Tokens:" << std::endl;
            for (const auto& token : tokens) {
                std::cout << token.to_string() << std::endl;
            }
            
            // Stage 2: Parsing
            Parser parser(tokens);
            std::shared_ptr<Program> program = parser.parse();
            
            // Print AST
            std::cout << "AST:" << std::endl;
            ASTPrinter printer;
            printer.print(program);
            
            // Stage 3: Interpretation/execution
            auto interpreter = std::make_shared<Interpreter>();
            JSValue result = interpreter->interpret(program);
            
            // Print result if it's not undefined
            if (!std::holds_alternative<std::monostate>(result)) {
                std::cout << "Result: " << interpreter->value_to_string(result) << std::endl;
            }
            
        } catch (const LexerError& error) {
            std::cerr << "Lexer Error: " << error.what() << std::endl;
            return EXIT_FAILURE;
        } catch (const ParserError& error) {
            std::cerr << "Parser Error: " << error.what() << std::endl;
            return EXIT_FAILURE;
        } catch (const RuntimeError& error) {
            std::cerr << "Runtime Error [line " << error.get_line() << ", column " << error.get_column() << "]: " 
                      << error.what() << std::endl;
            return EXIT_FAILURE;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << std::endl;
            return EXIT_FAILURE;
        }
    } else {
        // REPL mode
        std::cout << "Running in REPL mode (press Ctrl+D to exit)" << std::endl;
        
        // Create interpreter
        auto interpreter = std::make_shared<Interpreter>();
        
        // Start the REPL
        std::string input;
        std::string line;
        bool multiLineMode = false;
        int braceCount = 0;
        
        while (true) {
            if (multiLineMode) {
                std::cout << "... ";
            } else {
                std::cout << "> ";
            }
            
            if (!std::getline(std::cin, line)) {
                std::cout << std::endl << "Exiting..." << std::endl;
                break;
            }
            
            // Check for special commands (only in single-line mode)
            if (!multiLineMode) {
                if (line == ".help") {
                    std::cout << "Commands:\n"
                              << "  .help    Display this help message\n"
                              << "  .exit    Exit the REPL\n"
                              << "  Ctrl+D   Exit the REPL\n";
                    continue;
                } else if (line == ".exit") {
                    std::cout << "Exiting..." << std::endl;
                    break;
                }
            }
            
            // Skip empty lines in single-line mode
            if (line.empty() && !multiLineMode) {
                continue;
            }
            
            // Check if we need to enter multi-line mode
            if (!multiLineMode && is_multi_line_start(line)) {
                multiLineMode = true;
                input = line;
                
                // Count opening/closing braces
                for (char c : line) {
                    if (c == '{') braceCount++;
                    else if (c == '}') braceCount--;
                }
                
                // If the line doesn't end with a brace, we continue in multi-line mode
                if (braceCount > 0) {
                    continue;
                }
            } else if (multiLineMode) {
                // In multi-line mode, append the line
                input += "\n" + line;
                
                // Count braces
                for (char c : line) {
                    if (c == '{') braceCount++;
                    else if (c == '}') braceCount--;
                }
                
                // Continue if we still have unclosed braces
                if (braceCount > 0) {
                    continue;
                }
                
                // Exit multi-line mode if all braces are closed
                multiLineMode = false;
            } else {
                // Single-line mode
                input = line;
            }
            
            // Make sure the input ends with a semicolon
            if (!input.empty() && input.back() != ';') {
                input += ';';
            }
            
            try {
                // Evaluate the input
                JSValue result = interpreter->evaluate(input);
                
                // Print the result if it's not undefined
                if (!std::holds_alternative<std::monostate>(result)) {
                    std::cout << interpreter->value_to_string(result) << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
            
            // Reset state
            input.clear();
            braceCount = 0;
        }
    }
    
    return 0;
} 