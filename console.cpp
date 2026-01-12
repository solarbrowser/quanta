/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <chrono>

#ifdef _WIN32
#include <conio.h>
#endif

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

using namespace Quanta;

static const std::string RESET = "";
static const std::string BOLD = "";
static const std::string RED = "";
static const std::string GREEN = "";
static const std::string YELLOW = "";
static const std::string BLUE = "";
static const std::string MAGENTA = "";
static const std::string CYAN = "";

class QuantaConsole {
private:
    std::unique_ptr<Engine> engine_;
    
public:
    bool has_es6_module_syntax(const std::string& content) {
        std::istringstream stream(content);
        std::string line;
        
        while (std::getline(stream, line)) {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            
            std::string trimmed = line.substr(start);
            
            if (trimmed.substr(0, 6) == "import" || trimmed.substr(0, 6) == "export") {
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
        engine_ = std::make_unique<Engine>();
        bool init_result = engine_->initialize();
        
        if (!init_result) {
            std::cout << "Engine initialization failed!" << std::endl;
        }
    }
    
    bool execute_as_module(const std::string& filename, bool silent = false) {
        try {
            if (!silent) {
                std::cout << CYAN << "Auto-detected ES6 module syntax - loading as module..." << RESET << std::endl;
            }

            ModuleLoader* module_loader = engine_->get_module_loader();
            if (!module_loader) {
                if (!silent) {
                    std::cout << RED << "Error: ModuleLoader not available" << RESET << std::endl;
                }
                return false;
            }

            Module* module = module_loader->load_module(filename, "");
            if (module) {
                if (!silent) {
                    std::cout << GREEN << "Module loaded successfully!" << RESET << std::endl;
                }
                return true;
            } else {
                if (!silent) {
                    std::cout << RED << "Module loading failed!" << RESET << std::endl;
                }
                return false;
            }

        } catch (const std::exception& e) {
            if (!silent) {
                std::cout << RED << "Module execution error: " << e.what() << RESET << std::endl;
            }
            return false;
        }
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
    
    bool evaluate_expression(const std::string& input, bool show_prompt = true, bool show_result = true, const std::string& filename = "<console>") {
        try {
            auto start = std::chrono::high_resolution_clock::now();

            auto result = engine_->execute(input, filename);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            if (!result.has_value()) {
                auto& error = result.error();
                std::cout << RED;

                if (filename != "<console>" && (error.line_number > 0 || error.column_number > 0)) {
                    std::cout << filename;
                    if (error.line_number > 0) {
                        std::cout << ":" << error.line_number;
                        if (error.column_number > 0) {
                            std::cout << ":" << error.column_number;
                        }
                    }
                    std::cout << "\n";
                }

                std::cout << error.message << RESET << std::endl;
                return false;
            }

            if (show_result && !result->is_undefined()) {
                std::cout << GREEN << result->to_string() << RESET << std::endl;
            }
            
            
            return true;
        } catch (const std::exception& e) {
            std::cout << RED << "Error: " << e.what() << RESET << std::endl;
            return false;
        }
    }
    
    void clear_screen() {
        std::cout << "\033[2J\033[H";
    }
    
    std::string get_input() {
#ifdef USE_READLINE
        std::string prompt = GREEN + ">> " + RESET;
        char* line = readline(prompt.c_str());
        if (!line) return "";
        
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
            return "";
        }
        return input;
#endif
    }
    
    void run() {
        
        std::string input;
        while (true) {
            input = get_input();
            
            if (input.empty()) {
                break;
            }
            
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
                evaluate_expression(input);
            }
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        
        QuantaConsole console;
        
        bool execute_code = false;
        std::string code_to_execute;
        std::string filename;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if (arg == "-c" && i + 1 < argc) {
                execute_code = true;
                code_to_execute = argv[i + 1];
                i++;
                continue;
            } else if (arg.find("--") == 0) {
                continue;
            } else if (filename.empty()) {
                filename = arg;
            }
        }
        
        if (execute_code) {
            bool success = console.evaluate_expression(code_to_execute, false, true);

            EventLoop::instance().process_microtasks();

            return success ? 0 : 1;
        }

        if (!filename.empty()) {
            
            std::ifstream file(filename);
            if (!file.is_open()) {
                std::cerr << "Error: Cannot open file " << filename << std::endl;
                return 1;
            }
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            
            bool success = false;
            if (console.has_es6_module_syntax(content)) {
                success = console.execute_as_module(filename, true);
            } else {
                success = console.evaluate_expression(content, false, false, filename);
            }
            
            EventLoop::instance().process_microtasks();
            
            return success ? 0 : 1;
        }
        
        console.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}