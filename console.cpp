#include "core/include/Engine.h"
#include "core/include/Async.h"
#include "core/include/Generator.h"
#include "core/include/Iterator.h"
#include "core/include/ProxyReflect.h"
#include "lexer/include/Lexer.h"
#include "parser/include/Parser.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <chrono>

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
    // Helper function to detect if content uses ES6 module syntax
    bool has_es6_module_syntax(const std::string& content) {
        // Simple detection: look for import/export statements at the beginning of lines
        std::istringstream stream(content);
        std::string line;
        
        while (std::getline(stream, line)) {
            // Trim leading whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            
            std::string trimmed = line.substr(start);
            
            // Check for import/export statements
            if (trimmed.substr(0, 6) == "import" || trimmed.substr(0, 6) == "export") {
                // Make sure it's actually a keyword, not part of a string/comment
                if (trimmed.length() == 6 || std::isspace(trimmed[6]) || trimmed[6] == '{' || trimmed[6] == '*') {
                    return true;
                }
            }
        }
        return false;
    }
    
private:
    
public:
    QuantaConsole() {
        // Initialize engine with optimized configuration
        engine_ = std::make_unique<Engine>();
        bool init_result = engine_->initialize();  // Minimal lazy init!
        
        if (!init_result) {
            std::cout << "Engine initialization failed!" << std::endl;
        }
    }
    
    // Execute file as ES6 module
    void execute_as_module(const std::string& filename) {
        try {
            std::cout << CYAN << "Auto-detected ES6 module syntax - loading as module..." << RESET << std::endl;
            
            // Use the module loader to load and execute the file
            ModuleLoader* module_loader = engine_->get_module_loader();
            if (!module_loader) {
                std::cout << RED << "Error: ModuleLoader not available" << RESET << std::endl;
                return;
            }
            
            // Load the module (this will execute it)
            Module* module = module_loader->load_module(filename, "");
            if (module) {
                std::cout << GREEN << "Module loaded successfully!" << RESET << std::endl;
            } else {
                std::cout << RED << "Module loading failed!" << RESET << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cout << RED << "Module execution error: " << e.what() << RESET << std::endl;
        }
    }
    
    // for faster responses we are not not using this
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
            auto start = std::chrono::high_resolution_clock::now();
            
            auto result = engine_->execute(input, "<console>");
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            
            if (!result.success) {
                std::cout << RED << "Error: " << result.error_message << RESET << std::endl;
                return;
            }
            
            // Only show result if it's not undefined
            if (!result.value.is_undefined()) {
                std::cout << GREEN << result.value.to_string() << RESET << std::endl;
            }
            
            // debug only, for seeing execution time
            //std::cout << "Execution time: " << duration.count() << "ms" << std::endl;
            
            return;
        } catch (const std::exception& e) {
            std::cout << RED << "Error: " << e.what() << RESET << std::endl;
        }
    }
    
    void clear_screen() {
        std::cout << "\033[2J\033[H";
        // Fast mode - no banner
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
        // Fast mode - no banner
        
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
            // Fast mode - no banner
            
            std::ifstream file(argv[1]);
            if (!file.is_open()) {
                std::cerr << "Error: Cannot open file " << argv[1] << std::endl;
                return 1;
            }
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            
            // Auto-detect ES6 module syntax and choose execution method
            if (console.has_es6_module_syntax(content)) {
                // Execute as ES6 module
                console.execute_as_module(argv[1]);
            } else {
                // Execute as regular script
                console.evaluate_expression(content, true);
            }
            
            // Process any pending async tasks
            EventLoop::instance().process_microtasks();
            
            return 0;
        }
        
        console.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}