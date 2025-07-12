#include "core/include/Engine.h"
#include "lexer/include/Lexer.h"
#include "parser/include/Parser.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>

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
    int current_stage_;
    
public:
    QuantaConsole() : current_stage_(3) {
        engine_ = std::make_unique<Engine>();
        engine_->initialize();
        
        // Setup console object for console.log
        engine_->set_global_property("console", Value(ObjectFactory::create_object().release()));
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
        std::cout << YELLOW << "Current Stage: " << current_stage_ << " (Variables & Control Flow)\n" << RESET;
        std::cout << "\n";
    }
    
    void print_help() {
        std::cout << CYAN << BOLD << "Quanta Console Commands:\n" << RESET;
        std::cout << GREEN << "  .help" << RESET << "         - Show this help message\n";
        std::cout << GREEN << "  .quit/.exit" << RESET << "   - Exit the console\n";
        std::cout << GREEN << "  .stage <n>" << RESET << "    - Show stage information or switch stages\n";
        std::cout << GREEN << "  .test" << RESET << "        - Run comprehensive tests for current stage\n";
        std::cout << GREEN << "  .load <file>" << RESET << "  - Load and execute a JavaScript file\n";
        std::cout << GREEN << "  .tokens <expr>" << RESET << " - Show token analysis (Stage 1)\n";
        std::cout << GREEN << "  .ast <expr>" << RESET << "    - Show AST structure (Stage 2+)\n";
        std::cout << GREEN << "  .stats" << RESET << "       - Show engine performance statistics\n";
        std::cout << GREEN << "  .clear" << RESET << "       - Clear the screen\n";
        std::cout << "\n" << YELLOW << "Stage 2 Features:\n" << RESET;
        std::cout << "  • Mathematical operations: +, -, *, /, %, **\n";
        std::cout << "  • Comparison operators: ==, !=, ===, !==, <, >, <=, >=\n";
        std::cout << "  • Logical operators: &&, ||\n";
        std::cout << "  • Unary operators: +, -, !, ~, typeof\n";
        std::cout << "  • Parentheses grouping\n";
        std::cout << "  • console.log() function\n";
        std::cout << "  • Proper operator precedence\n";
        std::cout << "\n" << YELLOW << "Stage 3 Features:\n" << RESET;
        std::cout << "  • Variable declarations: var, let, const\n";
        std::cout << "  • Assignment operations\n";
        std::cout << "  • Block statements with {}\n";
        std::cout << "  • If/else control flow\n";
        std::cout << "  • File loading with .load command\n\n";
    }
    
    void show_stage_info(int stage = -1) {
        if (stage == -1) stage = current_stage_;
        
        std::cout << CYAN << BOLD << "Stage " << stage << " Information:\n" << RESET;
        
        switch (stage) {
            case 1:
                std::cout << YELLOW << "Stage 1: Lexical Analysis (Tokenizer)\n" << RESET;
                std::cout << "• Tokenizes JavaScript source code\n";
                std::cout << "• Supports all JavaScript tokens\n";
                std::cout << "• Position tracking for error reporting\n";
                std::cout << "• Unicode identifier support\n";
                break;
                
            case 2:
                std::cout << YELLOW << "Stage 2: Expression Parser & Evaluation\n" << RESET;
                std::cout << "• Full expression parsing with AST\n";
                std::cout << "• Mathematical operations with proper precedence\n";
                std::cout << "• console.log() implementation\n";
                std::cout << "• Type coercion and JavaScript semantics\n";
                break;
                
            case 3:
                std::cout << YELLOW << "Stage 3: Variables & Control Flow\n" << RESET;
                std::cout << "• Variable declarations (var, let, const)\n";
                std::cout << "• Assignment operations\n";
                std::cout << "• Block statements and scope\n";
                std::cout << "• If/else conditional statements\n";
                break;
                
            default:
                std::cout << RED << "Stage " << stage << " not implemented yet.\n" << RESET;
        }
        std::cout << "\n";
    }
    
    void run_tests() {
        std::cout << CYAN << BOLD << "Running Stage " << current_stage_ << " Tests...\n" << RESET;
        
        if (current_stage_ == 1) {
            run_stage1_tests();
        } else if (current_stage_ == 2) {
            run_stage2_tests();
        } else if (current_stage_ == 3) {
            run_stage3_tests();
        }
    }
    
    void run_stage1_tests() {
        std::cout << YELLOW << "Stage 1: Lexer Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            "42",
            "\"hello world\"",
            "console.log",
            "2 + 3 * 4",
            "function(x, y) { return x + y; }"
        };
        
        for (const auto& test : test_cases) {
            std::cout << GREEN << "Testing: " << RESET << test << "\n";
            show_tokens(test);
            std::cout << "\n";
        }
    }
    
    void run_stage2_tests() {
        std::cout << YELLOW << "Stage 2: Expression Parser Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            "2 + 3",
            "2 * 3 + 4",
            "2 + 3 * 4",
            "(2 + 3) * 4",
            "2 ** 3 ** 2",
            "true && false",
            "!true || false",
            "typeof 42",
            "console.log(\"Hello, Quanta!\")",
            "console.log(2 + 3, \"result\")"
        };
        
        for (const auto& test : test_cases) {
            std::cout << GREEN << "Testing: " << RESET << test << "\n";
            evaluate_expression(test, false);
            std::cout << "\n";
        }
    }
    
    void run_stage3_tests() {
        std::cout << YELLOW << "Stage 3: Variables & Control Flow Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            "var x = 5",
            "let y = 10",
            "const z = 15", 
            "var a = 2 + 3",
            "let b = true",
            "x = 42",
            "if (true) console.log(\"true branch\")",
            "if (false) console.log(\"false\") else console.log(\"else branch\")",
            "if (x > 0) { console.log(\"positive\"); console.log(x) }",
            "{ var local = 100; console.log(local) }"
        };
        
        for (const auto& test : test_cases) {
            std::cout << GREEN << "Testing: " << RESET << test << "\n";
            evaluate_expression(test, false);
            std::cout << "\n";
        }
    }
    
    void show_tokens(const std::string& input) {
        try {
            Lexer lexer(input);
            TokenSequence tokens = lexer.tokenize();
            
            std::cout << BLUE << "Tokens (" << tokens.size() << "):\n" << RESET;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const Token& token = tokens[i];
                std::cout << "  " << i + 1 << ". " << token.to_string() << "\n";
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
        if (current_stage_ < 2) {
            std::cout << YELLOW << "Expression evaluation requires Stage 2 or higher.\n" << RESET;
            std::cout << "Use .tokens to analyze tokens in Stage 1.\n";
            return;
        }
        
        try {
            // Use the parser for Stage 2+
            Lexer lexer(input);
            TokenSequence tokens = lexer.tokenize();
            
            if (tokens.size() == 0) {
                if (show_prompt) std::cout << MAGENTA << "undefined\n" << RESET;
                return;
            }
            
            Parser parser(tokens);
            std::unique_ptr<ASTNode> ast;
            
            // For Stage 3+, try parsing as statement first, fallback to expression
            if (current_stage_ >= 3) {
                ast = parser.parse_statement();
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
    
    void show_stats() {
        std::cout << CYAN << BOLD << "Engine Statistics:\n" << RESET;
        std::cout << engine_->get_performance_stats();
        std::cout << engine_->get_memory_stats();
    }
    
    void load_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cout << RED << "Error: Cannot open file '" << filename << "'" << RESET << "\n";
            return;
        }
        
        std::cout << CYAN << "Loading file: " << filename << RESET << "\n";
        
        std::string line;
        int line_number = 1;
        bool has_errors = false;
        
        while (std::getline(file, line)) {
            // Skip empty lines and comments
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            
            if (trimmed.empty() || trimmed.substr(0, 2) == "//") {
                line_number++;
                continue;
            }
            
            std::cout << BLUE << "Line " << line_number << ": " << RESET << trimmed << "\n";
            
            try {
                evaluate_expression(trimmed, false);
            } catch (const std::exception& e) {
                std::cout << RED << "Error on line " << line_number << ": " << e.what() << RESET << "\n";
                has_errors = true;
            }
            
            line_number++;
        }
        
        file.close();
        
        if (has_errors) {
            std::cout << YELLOW << "File loaded with errors.\n" << RESET;
        } else {
            std::cout << GREEN << "File loaded successfully!\n" << RESET;
        }
    }

    void clear_screen() {
        #ifdef _WIN32
            system("cls");
        #else
            system("clear");
        #endif
        print_banner();
    }
    
    std::string get_input() {
#ifdef USE_READLINE
        std::string prompt = GREEN + "quanta:" + std::to_string(current_stage_) + "> " + RESET;
        char* line = readline(prompt.c_str());
        if (!line) return ""; // EOF
        
        std::string input(line);
        if (!input.empty()) {
            add_history(line);
        }
        free(line);
        return input;
#else
        std::cout << GREEN << "quanta:" << current_stage_ << "> " << RESET;
        std::string input;
        if (!std::getline(std::cin, input)) {
            return ""; // EOF
        }
        return input;
#endif
    }

    void run() {
        print_banner();
        
#ifdef USE_READLINE
        std::cout << YELLOW << "Enhanced console with arrow keys and history enabled!\n" << RESET;
        std::cout << "Use ↑/↓ arrows for history, Ctrl+C to exit.\n\n";
#endif
        
        std::string input;
        while (true) {
            input = get_input();
            
            if (input.empty()) {
                break; // EOF
            }
            
            // Trim whitespace
            input.erase(0, input.find_first_not_of(" \t\n\r"));
            input.erase(input.find_last_not_of(" \t\n\r") + 1);
            
            if (input.empty()) {
                continue;
            }
            
            // Handle commands
            if (input[0] == '.') {
                std::istringstream iss(input);
                std::string command;
                iss >> command;
                
                if (command == ".help" || command == ".h") {
                    print_help();
                } else if (command == ".quit" || command == ".exit" || command == ".q") {
                    std::cout << CYAN << "Goodbye!\n" << RESET;
                    break;
                } else if (command == ".stage") {
                    std::string stage_str;
                    if (iss >> stage_str) {
                        try {
                            int stage = std::stoi(stage_str);
                            if (stage >= 1 && stage <= 3) {
                                current_stage_ = stage;
                                std::cout << GREEN << "Switched to Stage " << stage << "\n" << RESET;
                                show_stage_info(stage);
                            } else {
                                std::cout << RED << "Invalid stage. Available stages: 1-3\n" << RESET;
                            }
                        } catch (...) {
                            show_stage_info();
                        }
                    } else {
                        show_stage_info();
                    }
                } else if (command == ".test") {
                    run_tests();
                } else if (command == ".load") {
                    std::string filename;
                    if (iss >> filename) {
                        load_file(filename);
                    } else {
                        std::cout << YELLOW << "Usage: .load <filename>\n" << RESET;
                    }
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
                } else if (command == ".stats") {
                    show_stats();
                } else if (command == ".clear") {
                    clear_screen();
                } else {
                    std::cout << RED << "Unknown command: " << command << RESET << "\n";
                    std::cout << "Type " << BOLD << ".help" << RESET << " for available commands.\n";
                }
            } else {
                // Evaluate as expression/statement
                if (current_stage_ == 1) {
                    show_tokens(input);
                } else {
                    evaluate_expression(input);
                }
            }
        }
    }
};

int main() {
    try {
        QuantaConsole console;
        console.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}