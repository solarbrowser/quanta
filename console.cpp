#include "core/include/Engine.h"
#include "lexer/include/Lexer.h"
#include "parser/include/Parser.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>

#ifdef _WIN32
#include <conio.h>
#endif

// Optional readline support for better UX
#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

using namespace Quanta;

// ANSI color codes for better terminal output
static const std::string RESET = "\033[0m";
static const std::string BOLD = "\033[1m";
static const std::string RED = "\033[31m";
static const std::string GREEN = "\033[32m";
static const std::string YELLOW = "\033[33m";
static const std::string BLUE = "\033[34m";
static const std::string MAGENTA = "\033[35m";
static const std::string CYAN = "\033[36m";

class QuantaConsole {
private:
    std::unique_ptr<Engine> engine_;
    
public:
    QuantaConsole() {
        engine_ = std::make_unique<Engine>();
        engine_->initialize();
    }
    
    void print_banner() {
        std::cout << CYAN << BOLD;
        std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                      Quanta JavaScript Engine                 ║\n";
        std::cout << "║                        Interactive Console                    ║\n";
        std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
        std::cout << RESET;
        std::cout << "\n" << GREEN << "Welcome to Quanta! Type " << BOLD << ".help" << RESET << GREEN 
                  << " for commands, " << BOLD << ".quit" << RESET << GREEN << " to exit.\n" << RESET;
        std::cout << "\n";
    }
    
    void print_help() {
        std::cout << CYAN << BOLD << "Quanta Console Commands:\n" << RESET;
        std::cout << GREEN << "  .help" << RESET << "     - Show this help message\n";
        std::cout << GREEN << "  .quit" << RESET << "     - Exit the console\n";
        std::cout << GREEN << "  .clear" << RESET << "    - Clear the screen\n";
        std::cout << GREEN << "  .tokens" << RESET << "   - Show tokens for expression\n";
        std::cout << GREEN << "  .ast" << RESET << "      - Show AST for expression\n";
        std::cout << "\n" << YELLOW << "JavaScript Features Supported:\n" << RESET;
        std::cout << "• Variables (var, let, const), Functions, Objects, Arrays\n";
        std::cout << "• Control flow (if/else, loops, switch), Error handling (try/catch)\n";
        std::cout << "• Modules (import/export), Advanced operators (+=, ++, etc.)\n";
        std::cout << "• Built-in functions (console.log, etc.)\n";
        std::cout << "\n";
    }
    
    void show_tokens(const std::string& input) {
        try {
            Lexer lexer(input);
            TokenSequence tokens = lexer.tokenize();
            
            std::cout << BLUE << "Tokens:\n" << RESET;
            
            for (size_t i = 0; i < tokens.size(); ++i) {
                const Token& token = tokens[i];
                if (token.get_type() == TokenType::EOF_TOKEN) break;
                
                std::cout << "  " << i << ": " << YELLOW << token.type_name() << RESET 
                         << " '" << token.get_value() << "'\n";
            }
        } catch (const std::exception& e) {
            std::cout << RED << "Lexer error: " << e.what() << RESET << "\n";
        }
    }
    
    void show_ast(const std::string& input) {
        try {
            Lexer lexer(input);
            TokenSequence tokens = lexer.tokenize();
            
            Parser parser(tokens);
            auto ast = parser.parse_expression();
            
            std::cout << BLUE << "AST Structure:\n" << RESET;
            std::cout << "  " << ast->to_string() << "\n";
        } catch (const std::exception& e) {
            std::cout << RED << "Parser error: " << e.what() << RESET << "\n";
        }
    }
    
    void evaluate_expression(const std::string& input, bool show_prompt = true) {
        try {
            Lexer lexer(input);
            TokenSequence tokens = lexer.tokenize();
            
            if (tokens.size() == 0) {
                if (show_prompt) std::cout << MAGENTA << "undefined\n" << RESET;
                return;
            }
            
            Parser parser(tokens);
            std::unique_ptr<ASTNode> ast;
            
            // Try parsing as complete program first, fallback to expression
            auto program = parser.parse_program();
            if (program && program->get_statements().size() > 0) {
                // Execute all statements in the program
                Context* ctx = engine_->get_global_context();
                Value result;
                for (const auto& statement : program->get_statements()) {
                    result = statement->evaluate(*ctx);
                    if (ctx->has_exception()) {
                        break;
                    }
                }
                
                if (ctx->has_exception()) {
                    Value exception = ctx->get_exception();
                    std::cout << RED << "Error: " << exception.to_string() << RESET << "\n";
                    ctx->clear_exception();
                } else {
                    if (show_prompt) {
                        std::cout << MAGENTA << result.to_string() << RESET << "\n";
                    }
                }
                return;
            } else {
                ast = parser.parse_expression();
            }
            
            // Evaluate the AST
            Context* ctx = engine_->get_global_context();
            Value result = ast->evaluate(*ctx);
            
            if (ctx->has_exception()) {
                Value exception = ctx->get_exception();
                std::cout << RED << "Error: " << exception.to_string() << RESET << "\n";
                ctx->clear_exception();
            } else {
                if (show_prompt) {
                    std::cout << MAGENTA << result.to_string() << RESET << "\n";
                }
            }
            
        } catch (const std::exception& e) {
            std::cout << RED << "Error: " << e.what() << RESET << "\n";
        }
    }
    
    void clear_screen() {
        std::cout << "\033[2J\033[H";
        print_banner();
    }
    
    std::string get_input() {
#ifdef USE_READLINE
        std::string prompt = GREEN + ">> " + RESET;
        char* line = readline(prompt.c_str());
        if (!line) return ""; // EOF
        
        std::string input(line);
        if (!input.empty()) {
            add_history(line);
        }
        free(line);
        return input;
#else
        std::cout << GREEN << ">> " << RESET;
        std::string input;
        if (!std::getline(std::cin, input)) {
            return ""; // EOF
        }
        return input;
#endif
    }
    
    void run() {
        print_banner();
        
        std::string input;
        while (true) {
            input = get_input();
            
            if (input.empty()) {
                break; // EOF
            }
            
            // Handle commands
            if (input[0] == '.') {
                std::istringstream iss(input);
                std::string command;
                iss >> command;
                
                if (command == ".quit" || command == ".exit") {
                    std::cout << CYAN << "Goodbye!\n" << RESET;
                    break;
                } else if (command == ".help") {
                    print_help();
                } else if (command == ".tokens") {
                    std::string rest;
                    std::getline(iss, rest);
                    if (!rest.empty()) {
                        rest.erase(0, rest.find_first_not_of(" \t"));
                        show_tokens(rest);
                    } else {
                        std::cout << YELLOW << "Usage: .tokens <expression>\n" << RESET;
                    }
                } else if (command == ".ast") {
                    std::string rest;
                    std::getline(iss, rest);
                    if (!rest.empty()) {
                        rest.erase(0, rest.find_first_not_of(" \t"));
                        show_ast(rest);
                    } else {
                        std::cout << YELLOW << "Usage: .ast <expression>\n" << RESET;
                    }
                } else if (command == ".clear") {
                    clear_screen();
                } else {
                    std::cout << RED << "Unknown command: " << command << RESET << "\n";
                    std::cout << "Type " << BOLD << ".help" << RESET << " for available commands.\n";
                }
            } else {
                // Evaluate as expression/statement
                evaluate_expression(input);
            }
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        QuantaConsole console;
        
        // If file argument provided, execute it instead of interactive mode
        if (argc > 1) {
            // Show banner for file execution too
            console.print_banner();
            
            std::ifstream file(argv[1]);
            if (!file.is_open()) {
                std::cerr << "Error: Cannot open file " << argv[1] << std::endl;
                return 1;
            }
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            
            // Execute the file content
            console.evaluate_expression(content, true);
            
            // Wait for user input before closing
            std::cout << "\nPress any key to exit...";
            std::cout.flush();
#ifdef _WIN32
            _getch();
#else
            std::cin.get();
#endif
            
            return 0;
        }
        
        console.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}