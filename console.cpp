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
    QuantaConsole() : current_stage_(7) {
        engine_ = std::make_unique<Engine>();
        engine_->initialize();
        
        // Console is already set up in engine initialization
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
                
            case 4:
                std::cout << YELLOW << "Stage 4: Functions\n" << RESET;
                std::cout << "• Function declarations\n";
                std::cout << "• Function expressions\n";
                std::cout << "• Function calls with parameters\n";
                std::cout << "• Return statements\n";
                std::cout << "• Local scope and closures\n";
                std::cout << "• Recursive functions\n";
                break;
                
            case 5:
                std::cout << YELLOW << "Stage 5: Loops\n" << RESET;
                std::cout << "• for loops\n";
                std::cout << "• while loops\n";
                std::cout << "• do-while loops\n";
                std::cout << "• break and continue\n";
                std::cout << "• Nested loops\n";
                break;
                
            case 6:
                std::cout << YELLOW << "Stage 6: Objects\n" << RESET;
                std::cout << "• Object literals {key: value}\n";
                std::cout << "• Property access obj.prop and obj['prop']\n";
                std::cout << "• Property assignment obj.prop = value\n";
                std::cout << "• Object methods obj.method()\n";
                std::cout << "• Nested objects\n";
                std::cout << "• Dynamic property names\n";
                break;
                
            case 7:
                std::cout << YELLOW << "Stage 7: Arrays\n" << RESET;
                std::cout << "• Array literals [1, 2, 3]\n";
                std::cout << "• Array indexing arr[0] and arr[i]\n";
                std::cout << "• Array assignment arr[0] = value\n";
                std::cout << "• Array length property arr.length\n";
                std::cout << "• Nested arrays [[1, 2], [3, 4]]\n";
                std::cout << "• Mixed type arrays [1, \"str\", obj]\n";
                std::cout << "• Array methods push, pop, etc\n";
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
        } else if (current_stage_ == 4) {
            run_stage4_tests();
        } else if (current_stage_ == 5) {
            run_stage5_tests();
        } else if (current_stage_ == 6) {
            run_stage6_tests();
        } else if (current_stage_ == 7) {
            run_stage7_tests();
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
    
    void run_stage4_tests() {
        std::cout << YELLOW << "Stage 4: Functions Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            "function greet() { console.log(\"Hello!\") }",
            "function add(x, y) { return x + y }",
            "var square = function(n) { return n * n }",
            "function factorial(n) { if (n <= 1) return 1; return n * factorial(n - 1) }",
            "greet()",
            "add(5, 3)",
            "square(4)",
            "factorial(5)",
            "function outer() { var x = 10; function inner() { return x; } return inner; }",
            "var result = outer(); result()"
        };
        
        for (const auto& test : test_cases) {
            std::cout << GREEN << "Testing: " << RESET << test << "\n";
            evaluate_expression(test, false);
            std::cout << "\n";
        }
    }
    
    void run_stage5_tests() {
        std::cout << YELLOW << "Stage 5: Loops Tests\n" << RESET;
        std::cout << RED << "Stage 5 not implemented yet - use .stage 6 for objects!\n" << RESET;
    }
    
    void run_stage6_tests() {
        std::cout << YELLOW << "Stage 6: Objects Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            // Object literals
            "var obj = {name: \"Alice\", age: 25}",
            "var empty = {}",
            "({x: 1, y: 2})",
            
            // Property access
            "obj.name",
            "obj[\"age\"]", 
            "obj.name",
            
            // Property assignment
            "obj.city = \"Boston\"",
            "obj[\"country\"] = \"USA\"",
            "empty.newProp = \"added\"",
            
            // Nested objects
            "var person = {name: \"Bob\", address: {street: \"123 Main\", city: \"NYC\"}}",
            "person.address.city",
            "person.address.zip = \"10001\"",
            
            // Object methods
            "var calculator = {add: function(a, b) { return a + b }}",
            "calculator.add",
            "calculator.add(5, 3)",
            
            // Dynamic properties
            "var key = \"dynamic\"",
            "obj[key] = \"value\"",
            "obj[key]",
            
            // Method calls with brackets
            "var api = {getData: function() { return \"data\" }}",
            "api[\"getData\"]()"
        };
        
        for (const auto& test : test_cases) {
            std::cout << GREEN << "Testing: " << RESET << test << "\n";
            evaluate_expression(test, false);
            std::cout << "\n";
        }
    }
    
    void run_stage7_tests() {
        std::cout << YELLOW << "Stage 7: Arrays Tests\n" << RESET;
        
        std::vector<std::string> test_cases = {
            // Array literals
            "[1, 2, 3]",
            "var arr = [1, 2, 3]",
            "var empty = []",
            
            // Array indexing
            "arr[0]",
            "arr[1]", 
            "arr[2]",
            "empty.length",
            "arr.length",
            
            // Array assignment
            "arr[0] = 10",
            "arr[0]",
            "arr[3] = 4",
            "arr.length",
            
            // Nested arrays
            "var nested = [[1, 2], [3, 4]]",
            "nested[0]",
            "nested[0][1]",
            "nested[1][0]",
            
            // Mixed type arrays
            "var mixed = [1, \"hello\", {name: \"test\"}, [5, 6]]",
            "mixed[1]",
            "mixed[2].name",
            "mixed[3][0]",
            "mixed.length",
            
            // Dynamic indexing
            "var index = 1",
            "arr[index]",
            "mixed[index + 1]",
            
            // Array of objects
            "var people = [{name: \"Alice\"}, {name: \"Bob\"}]",
            "people[0].name",
            "people[1].name",
            
            // Sparse arrays
            "[1, , 3]",
            "var sparse = [1, , 3]",
            "sparse[1]",
            
            // Array methods
            "var methods = [1, 2, 3]",
            "methods.push(4)",
            "methods.length",
            "methods.pop()",
            "methods.shift()", 
            "methods.unshift(0)",
            "methods.join(\", \")",
            "methods.indexOf(2)"
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
            
            // For Stage 3+, try parsing as complete program first, fallback to expression
            if (current_stage_ >= 3) {
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
                    ast = parser.parse_statement();
                }
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
                            if (stage >= 1 && stage <= 7) {
                                current_stage_ = stage;
                                std::cout << GREEN << "Switched to Stage " << stage << "\n" << RESET;
                                show_stage_info(stage);
                            } else {
                                std::cout << RED << "Invalid stage. Available stages: 1-7\n" << RESET;
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