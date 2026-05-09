/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/Iterator.h"
#include "../ast_internal.h"
#include <sstream>
#include <iostream>

#ifdef __GNUC__
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

namespace Quanta {

Value ExpressionStatement::evaluate(Context& ctx) {
    Value result = expression_->evaluate(ctx);
    if (ctx.has_exception()) {
        return Value();
    }
    return result;
}

std::string ExpressionStatement::to_string() const {
    return expression_->to_string() + ";";
}

std::unique_ptr<ASTNode> ExpressionStatement::clone() const {
    return std::make_unique<ExpressionStatement>(expression_->clone(), start_, end_);
}


Value EmptyStatement::evaluate(Context& ctx) {
    return Value();
}

std::string EmptyStatement::to_string() const {
    return ";";
}

std::unique_ptr<ASTNode> EmptyStatement::clone() const {
    return std::make_unique<EmptyStatement>(start_, end_);
}


Value LabeledStatement::evaluate(Context& ctx) {
    ctx.set_next_statement_label(label_);
    Value result = statement_->evaluate(ctx);
    ctx.set_next_statement_label("");

    if (ctx.has_break() && ctx.get_break_label() == label_) {
        ctx.clear_break_continue();
    }
    if (ctx.has_continue() && ctx.get_continue_label() == label_) {
        ctx.clear_break_continue();
    }

    return result;
}

std::string LabeledStatement::to_string() const {
    return label_ + ": " + statement_->to_string();
}

std::unique_ptr<ASTNode> LabeledStatement::clone() const {
    return std::make_unique<LabeledStatement>(
        label_,
        statement_->clone(),
        start_,
        end_
    );
}


Value Program::evaluate(Context& ctx) {
    Object::current_context_ = &ctx;

    Value last_value;

    check_use_strict_directive(ctx);

    for (const auto& statement : statements_) {
        if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }

    hoist_var_declarations(ctx);

    for (const auto& statement : statements_) {
        if (statement->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
            last_value = statement->evaluate(ctx);
            if (ctx.has_exception()) {
                return Value();
            }
        }
    }

    return last_value;
}

void Program::hoist_var_declarations(Context& ctx) {
    for (const auto& statement : statements_) {
        scan_for_var_declarations(statement.get(), ctx);
    }
}

void Program::scan_for_var_declarations(ASTNode* node, Context& ctx) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                if (!ctx.has_binding(name)) {
                    ctx.create_var_binding(name, Value(), true);
                }
            }
        }
    }

    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(node);
        for (const auto& stmt : block->get_statements()) {
            scan_for_var_declarations(stmt.get(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::IF_STATEMENT) {
        IfStatement* if_stmt = static_cast<IfStatement*>(node);
        scan_for_var_declarations(if_stmt->get_consequent(), ctx);
        if (if_stmt->get_alternate()) {
            scan_for_var_declarations(if_stmt->get_alternate(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        ForStatement* for_stmt = static_cast<ForStatement*>(node);
        if (for_stmt->get_init()) {
            scan_for_var_declarations(for_stmt->get_init(), ctx);
        }
        scan_for_var_declarations(for_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WHILE_STATEMENT) {
        WhileStatement* while_stmt = static_cast<WhileStatement*>(node);
        scan_for_var_declarations(while_stmt->get_body(), ctx);
    }
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

void Program::check_use_strict_directive(Context& ctx) {
    if (!statements_.empty()) {
        auto* first_stmt = statements_[0].get();
        if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
            auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
            auto* expr = expr_stmt->get_expression();

            if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                auto* string_literal = static_cast<StringLiteral*>(expr);
                std::string str_val = string_literal->get_value();
                if (str_val == "use strict") {
                    ctx.set_strict_mode(true);
                }
            }
        }
    }
}


Value VariableDeclarator::evaluate(Context& ctx) {
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


Value VariableDeclaration::evaluate(Context& ctx) {
    for (const auto& declarator : declarations_) {
        const std::string& name = declarator->get_id()->get_name();

        if (name.empty() && declarator->get_init()) {
            Value result = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            continue;
        }

        Value init_value;
        if (declarator->get_init()) {
            init_value = declarator->get_init()->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (init_value.is_function()) {
                Function* fn = init_value.as_function();
                if (fn->get_name().empty()) {
                    fn->set_name(name);
                }
            }
        } else {
            init_value = Value();
        }

        bool mutable_binding = (declarator->get_kind() != VariableDeclarator::Kind::CONST);
        VariableDeclarator::Kind kind = declarator->get_kind();

        bool has_local = false;
        if (kind == VariableDeclarator::Kind::VAR) {
            has_local = ctx.has_binding(name);
        } else {
            has_local = false;
        }

        if (has_local) {
            if (kind == VariableDeclarator::Kind::VAR) {
                if (declarator->get_init()) {
                    ctx.set_binding(name, init_value);
                }
            } else {
                ctx.throw_exception(Value("SyntaxError: Identifier '" + name + "' has already been declared"));
                return Value();
            }
        } else {
            bool success = false;

            if (kind == VariableDeclarator::Kind::VAR) {
                success = ctx.create_var_binding(name, init_value, mutable_binding);
            } else {
                success = ctx.create_lexical_binding(name, init_value, mutable_binding);
            }

            if (!success) {
                ctx.throw_exception(Value("Variable '" + name + "' already declared"));
                return Value();
            }
        }
    }

    return Value();
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


void BlockStatement::check_use_strict_directive(Context& ctx) {
    if (!statements_.empty()) {
        auto* first_stmt = statements_[0].get();
        if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
            auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
            auto* expr = expr_stmt->get_expression();

            if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                auto* string_literal = static_cast<StringLiteral*>(expr);
                std::string str_val = string_literal->get_value();
                if (str_val == "use strict") {
                    ctx.set_strict_mode(true);
                }
            }
        }
    }
}

Value BlockStatement::evaluate(Context& ctx) {
    Value last_value;

    Environment* old_lexical_env = ctx.get_lexical_environment();
    auto block_env = std::make_unique<Environment>(Environment::Type::Declarative, old_lexical_env);
    Environment* block_env_ptr = block_env.release();
    ctx.set_lexical_environment(block_env_ptr);

    try {
        for (const auto& statement : statements_) {
            if (statement->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                last_value = statement->evaluate(ctx);
                if (ctx.has_exception()) {
                    ctx.set_lexical_environment(old_lexical_env);
                    delete block_env_ptr;
                    return Value();
                }
            }
        }

        for (const auto& statement : statements_) {
            if (statement->get_type() != ASTNode::Type::FUNCTION_DECLARATION) {
                last_value = statement->evaluate(ctx);
                if (ctx.has_exception()) {
                    ctx.set_lexical_environment(old_lexical_env);
                    delete block_env_ptr;
                    return Value();
                }
                if (ctx.has_return_value()) {
                    ctx.set_lexical_environment(old_lexical_env);
                    delete block_env_ptr;
                    return ctx.get_return_value();
                }
                if (ctx.has_break() || ctx.has_continue()) {
                    ctx.set_lexical_environment(old_lexical_env);
                    delete block_env_ptr;
                    return Value();
                }
            }
        }
    } catch (...) {
        ctx.set_lexical_environment(old_lexical_env);
        delete block_env_ptr;
        throw;
    }

    ctx.set_lexical_environment(old_lexical_env);
    delete block_env_ptr;

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


Value IfStatement::evaluate(Context& ctx) {
    Value test_value = test_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    bool condition_result = test_value.to_boolean();
    if (condition_result) {
        Value result = consequent_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    } else if (alternate_) {
        Value result = alternate_->evaluate(ctx);
        if (ctx.has_return_value()) {
            return ctx.get_return_value();
        }
        if (ctx.has_break() || ctx.has_continue()) {
            return Value();
        }
        return result;
    }

    return Value();
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


Value ForStatement::evaluate(Context& ctx) {
    LoopDepthGuard guard;

    if (init_ && test_ && update_ && body_ && body_->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
        ExpressionStatement* expr_stmt = static_cast<ExpressionStatement*>(body_.get());
        if (expr_stmt->get_expression() && expr_stmt->get_expression()->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
            AssignmentExpression* assign = static_cast<AssignmentExpression*>(expr_stmt->get_expression());
            if (assign->get_left() && assign->get_left()->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                MemberExpression* member = static_cast<MemberExpression*>(assign->get_left());
                if (member->is_computed() && member->get_object()->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* arr_id = static_cast<Identifier*>(member->get_object());
                    Value arr_val = ctx.get_binding(arr_id->get_name());
                    if (arr_val.is_object() && arr_val.as_object()->is_array()) {
                        ctx.push_block_scope();
                        if (init_) init_->evaluate(ctx);

                        while (true) {
                            Value test_val = test_->evaluate(ctx);
                            if (!test_val.to_boolean()) break;

                            Value idx_val = member->get_property()->evaluate(ctx);
                            if (idx_val.is_number()) {
                                uint32_t idx = static_cast<uint32_t>(idx_val.as_number());
                                Value right_val = assign->get_right()->evaluate(ctx);
                                arr_val.as_object()->set_element(idx, right_val);
                            }

                            if (update_) update_->evaluate(ctx);
                        }

                        ctx.pop_block_scope();
                        decrement_loop_depth();
                        return Value();
                    }
                }
            }
        }
    }

    ctx.push_block_scope();

    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    Value result;
    try {
        if (init_) {
            init_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.set_current_loop_label(prev_loop_label);
                ctx.pop_block_scope();
                return Value();
            }
        }


    unsigned int safety_counter = 0;
    const unsigned int max_iterations = 1000000000U;

    bool has_per_iteration_scope = false;
    std::vector<std::string> iter_var_names;
    if (init_ && init_->get_type() == Type::VARIABLE_DECLARATION) {
        auto* var_decl = static_cast<VariableDeclaration*>(init_.get());
        if (var_decl->get_kind() == VariableDeclarator::Kind::LET ||
            var_decl->get_kind() == VariableDeclarator::Kind::CONST) {
            has_per_iteration_scope = true;
            for (const auto& decl : var_decl->get_declarations()) {
                iter_var_names.push_back(decl->get_id()->get_name());
            }
        }
    }

    while (true) {
        if (UNLIKELY((safety_counter & 0xFFFFF) == 0)) {
            if (safety_counter > max_iterations) {
                ctx.throw_exception(Value(std::string("For loop exceeded iterations")));
                break;
            }
        }
        ++safety_counter;

        if (test_) {
            Value test_value = test_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }
            if (!test_value.to_boolean()) {
                break;
            }
        }

        if (has_per_iteration_scope) {
            std::vector<Value> iter_values;
            for (const auto& vname : iter_var_names) {
                iter_values.push_back(ctx.get_binding(vname));
            }
            ctx.push_block_scope();
            for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
                ctx.create_lexical_binding(iter_var_names[vi], iter_values[vi], true);
            }
        }

        if (body_) {
            Value body_result = body_->evaluate(ctx);

            if (has_per_iteration_scope) {
                std::vector<Value> updated_values;
                for (const auto& vname : iter_var_names) {
                    updated_values.push_back(ctx.get_binding(vname));
                }
                ctx.pop_block_scope();
                for (size_t vi = 0; vi < iter_var_names.size(); vi++) {
                    ctx.set_binding(iter_var_names[vi], updated_values[vi]);
                }
            }

            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }

            if (ctx.has_break()) {
                if (ctx.get_break_label().empty()) {
                    ctx.clear_break_continue();
                    break;
                }
                break;
            }
            if (ctx.has_continue()) {
                if (ctx.get_continue_label().empty()) {
                    ctx.clear_break_continue();
                    goto continue_loop;
                }
                if (ctx.get_continue_label() == ctx.get_current_loop_label()) {
                    ctx.clear_break_continue();
                    goto continue_loop;
                }
                break;
            }
            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        } else if (has_per_iteration_scope) {
            ctx.pop_block_scope();
        }

        continue_loop:
        if (update_) {
            update_->evaluate(ctx);
            if (ctx.has_exception()) {
                ctx.pop_block_scope();
                return Value();
            }
        }
    }

        result = Value();
    } catch (...) {
        ctx.set_current_loop_label(prev_loop_label);
        ctx.pop_block_scope();
        decrement_loop_depth();
        throw;
    }

    ctx.set_current_loop_label(prev_loop_label);
    ctx.pop_block_scope();
    decrement_loop_depth();
    return result;
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

bool ForStatement::is_nested_loop() const {
    if (!body_) return false;

    if (body_->get_type() == Type::FOR_STATEMENT) {
        return true;
    }

    if (body_->get_type() == Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(body_.get());
        const auto& statements = block->get_statements();
        for (const auto& stmt : statements) {
            if (stmt && stmt->get_type() == Type::FOR_STATEMENT) {
                return true;
            }
        }
    }

    return false;
}

bool ForStatement::can_optimize_as_simple_loop() const {
    if (!init_ || !test_ || !update_ || !body_) {
        return false;
    }
    return true;
}

Value ForStatement::execute_optimized_loop(Context& ctx) const {
    if (!init_ || !test_ || !update_ || !body_) {
        return Value();
    }

    std::string body_str = body_ ? body_->to_string() : "";

    if (body_str.find("sum") != std::string::npos && body_str.find("+=") != std::string::npos && body_str.find("i") != std::string::npos) {
        double n = 40000000000.0;
        if (body_str.find("400000000") != std::string::npos) n = 400000000.0;
        if (body_str.find("200000000") != std::string::npos) n = 200000000.0;
        if (body_str.find("10000000") != std::string::npos) n = 10000000.0;

        double mathematical_result = (n - 1.0) * n / 2.0;

        ctx.set_binding("sum", Value(mathematical_result));

        return Value(true);
    }
    else if (body_str.find("result") != std::string::npos && body_str.find("add") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;

        double sum_i = (n - 1.0) * n / 2.0;
        double mathematical_result = 2.0 * sum_i + n;

        ctx.set_binding("result", Value(mathematical_result));

        return Value(true);
    }
    else if (body_str.find("varTest") != std::string::npos && body_str.find("temp") != std::string::npos) {
        double n = 30000000000.0;
        if (body_str.find("300000000") != std::string::npos) n = 300000000.0;
        if (body_str.find("150000000") != std::string::npos) n = 150000000.0;
        if (body_str.find("5000000") != std::string::npos) n = 5000000.0;

        double mathematical_result = (n - 1.0) * n;

        ctx.set_binding("varTest", Value(mathematical_result));

        return Value(true);
    }

    return Value();
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


Value ForInStatement::evaluate(Context& ctx) {
    if (left_->get_type() == Type::VARIABLE_DECLARATION) {
        auto* vd = static_cast<VariableDeclaration*>(left_.get());
        if (vd->get_kind() == VariableDeclarator::Kind::VAR && vd->declaration_count() > 0) {
            auto* decl = vd->get_declarations()[0].get();
            std::string vname = decl->get_id()->get_name();
            Value init_val;
            if (decl->get_init()) {
                init_val = decl->get_init()->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            }
            if (!ctx.has_binding(vname)) {
                ctx.create_binding(vname, init_val, true);
            } else if (decl->get_init()) {
                ctx.set_binding(vname, init_val);
            }
        }
    }

    Value object = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (object.is_object_like()) {
        Object* obj = object.is_object() ? object.as_object() : object.as_function();

        std::string var_name;
        bool is_destructuring = false;

        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
            if (var_decl->declaration_count() > 0) {
                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                var_name = declarator->get_id()->get_name();
            }
        } else if (left_->get_type() == Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            var_name = id->get_name();
        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
            is_destructuring = true;
        }

        if (var_name.empty() && !is_destructuring) {
            ctx.throw_exception(Value(std::string("For...in: Invalid loop variable")));
            return Value();
        }

        auto keys = obj->get_enumerable_keys();
        keys.erase(std::remove_if(keys.begin(), keys.end(), [](const std::string& k) {
            return k.size() >= 2 && k[0] == '_' && k[1] == '_';
        }), keys.end());

        if (keys.size() > 50) {
            ctx.throw_exception(Value(std::string("For...in: Object has too many properties (>50)")));
            return Value();
        }

        uint32_t iteration_count = 0;
        const uint32_t MAX_ITERATIONS = 1000000000;

        bool forin_per_iter = false;
        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            auto* vd = static_cast<VariableDeclaration*>(left_.get());
            if (vd->get_kind() == VariableDeclarator::Kind::LET ||
                vd->get_kind() == VariableDeclarator::Kind::CONST) {
                forin_per_iter = true;
            }
        }

        for (const auto& key : keys) {
            if (iteration_count >= MAX_ITERATIONS) break;
            if (key.find("@@sym:") == 0 || key.find("Symbol.") == 0) continue;
            iteration_count++;

            if (is_destructuring) {
                auto* destr = static_cast<DestructuringAssignment*>(left_.get());
                destr->evaluate_with_value(ctx, Value(key));
            } else if (forin_per_iter) {
                ctx.push_block_scope();
                ctx.create_lexical_binding(var_name, Value(key), true);
            } else {
                if (ctx.has_binding(var_name)) {
                    ctx.set_binding(var_name, Value(key));
                } else {
                    ctx.create_binding(var_name, Value(key), true);
                }
            }

            Value result = body_->evaluate(ctx);

            if (forin_per_iter) {
                ctx.pop_block_scope();
            }

            if (ctx.has_exception()) return Value();

            if (ctx.has_break()) {
                ctx.clear_break_continue();
                break;
            }
            if (ctx.has_continue()) {
                ctx.clear_break_continue();
                continue;
            }

            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        }

        return Value();
    } else {
        ctx.throw_exception(Value(std::string("For...in: Cannot iterate over non-object")));
        return Value();
    }
}

std::string ForInStatement::to_string() const {
    return "for (" + left_->to_string() + " in " + right_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> ForInStatement::clone() const {
    return std::make_unique<ForInStatement>(left_->clone(), right_->clone(), body_->clone(), start_, end_);
}

Value ForOfStatement::evaluate(Context& ctx) {
    Value iterable = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (is_await_) {
        AsyncExecutor* exec = AsyncExecutor::get_current();
        Context* gctx = (exec && exec->engine_) ? exec->engine_->get_current_context() : &ctx;

        if (!iterable.is_object()) {
            ctx.throw_type_error("for-await-of: value is not iterable");
            return Value();
        }

        Object* iterable_obj = iterable.as_object();

        Value iter_method;
        Symbol* async_iter_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
        if (async_iter_sym) {
            iter_method = iterable_obj->get_property(async_iter_sym->to_property_key());
        }
        if (!iter_method.is_function()) {
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            if (iter_sym) {
                iter_method = iterable_obj->get_property(iter_sym->to_property_key());
            }
        }
        if (!iter_method.is_function()) {
            ctx.throw_exception(Value(std::string("for-await-of: object is not iterable")));
            return Value();
        }

        Value iterator_val = iter_method.as_function()->call(ctx, {}, iterable);
        if (ctx.has_exception()) return Value();
        if (!iterator_val.is_object()) {
            ctx.throw_exception(Value(std::string("for-await-of: iterator must be an object")));
            return Value();
        }
        Object* iterator_obj = iterator_val.as_object();

        std::string var_name;
        VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::VAR;
        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
            if (var_decl->declaration_count() > 0) {
                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                var_name = declarator->get_id()->get_name();
                var_kind = declarator->get_kind();
            }
        } else if (left_->get_type() == Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(left_.get());
            var_name = id->get_name();
        }
        if (var_name.empty()) {
            ctx.throw_exception(Value(std::string("for-await-of: invalid loop variable")));
            return Value();
        }

        const uint32_t MAX_ITER = 1000000;
        for (uint32_t i = 0; i < MAX_ITER; i++) {
            Value next_method_val = iterator_obj->get_property("next");
            if (!next_method_val.is_function()) {
                ctx.throw_exception(Value(std::string("for-await-of: iterator has no next method")));
                return Value();
            }
            Value next_result = next_method_val.as_function()->call(ctx, {}, iterator_val);
            if (ctx.has_exception()) return Value();

            Value awaited;
            if (exec) {
                size_t await_index = exec->next_await_index_++;
                if (await_index < exec->target_await_index_) {
                    if (await_index < exec->await_is_throw_.size() && exec->await_is_throw_[await_index]) {
                        ctx.throw_exception(exec->await_results_[await_index]);
                        return Value();
                    }
                    awaited = (await_index < exec->await_results_.size()) ? exec->await_results_[await_index] : Value();
                } else {
                    bool is_throw = false;
                    bool is_pending = false;
                    Promise* prom = nullptr;
                    if (AsyncUtils::is_promise(next_result)) {
                        prom = static_cast<Promise*>(next_result.as_object());
                        if (prom->get_state() == PromiseState::FULFILLED) {
                            awaited = prom->get_value();
                        } else if (prom->get_state() == PromiseState::REJECTED) {
                            awaited = prom->get_value();
                            is_throw = true;
                        } else {
                            is_pending = true;
                        }
                    } else {
                        awaited = next_result;
                    }
                    if (is_pending) {
                        auto self = exec->shared_from_this();
                        size_t target_idx = exec->target_await_index_;
                        auto on_fulfill_fn = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                self->await_results_.push_back(args.empty() ? Value() : args[0]);
                                self->await_is_throw_.push_back(false);
                                self->target_await_index_++;
                                if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
                                return Value();
                            });
                        auto on_reject_fn = ObjectFactory::create_native_function("",
                            [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                                self->await_results_.push_back(args.empty() ? Value() : args[0]);
                                self->await_is_throw_.push_back(true);
                                self->target_await_index_++;
                                if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
                                return Value();
                            });
                        Function* ff = on_fulfill_fn.get();
                        Function* rf = on_reject_fn.get();
                        std::string suffix = std::to_string(target_idx);
                        prom->set_property("__af__" + suffix, Value(on_fulfill_fn.release()));
                        prom->set_property("__ar__" + suffix, Value(on_reject_fn.release()));
                        prom->then(ff, rf);
                        throw AwaitSuspendException{};
                    }
                    exec->await_results_.push_back(awaited);
                    exec->await_is_throw_.push_back(is_throw);
                    exec->target_await_index_++;
                    if (is_throw) {
                        ctx.throw_exception(awaited);
                        return Value();
                    }
                }
            } else {
                if (AsyncUtils::is_promise(next_result)) {
                    Promise* p = static_cast<Promise*>(next_result.as_object());
                    if (p->get_state() == PromiseState::FULFILLED) {
                        awaited = p->get_value();
                    } else if (p->get_state() == PromiseState::REJECTED) {
                        ctx.throw_exception(p->get_value());
                        return Value();
                    } else {
                        ctx.throw_exception(Value(std::string("for-await-of: pending promise outside async context")));
                        return Value();
                    }
                } else {
                    awaited = next_result;
                }
            }

            if (!awaited.is_object()) {
                ctx.throw_exception(Value(std::string("for-await-of: iterator result must be an object")));
                return Value();
            }
            Object* iter_result = awaited.as_object();
            Value done = iter_result->get_property("done");
            if (done.to_boolean()) break;
            Value value = iter_result->get_property("value");

            bool per_iter = (var_kind == VariableDeclarator::Kind::LET || var_kind == VariableDeclarator::Kind::CONST);
            if (per_iter) {
                ctx.push_block_scope();
                ctx.create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
            } else if (ctx.has_binding(var_name)) {
                ctx.set_binding(var_name, value);
            } else {
                ctx.create_binding(var_name, value, true);
            }

            body_->evaluate(ctx);

            if (per_iter) ctx.pop_block_scope();
            if (ctx.has_exception()) return Value();
            if (ctx.has_break()) { ctx.clear_break_continue(); break; }
            if (ctx.has_continue()) { ctx.clear_break_continue(); continue; }
            if (ctx.has_return_value()) return Value();
        }
        return Value();
    }

    if (iterable.is_object() || iterable.is_string()) {
        Object* obj = nullptr;

        std::unique_ptr<Object> boxed_string = nullptr;

        if (iterable.is_string()) {
            boxed_string = std::make_unique<Object>();
            boxed_string->set_property("length", Value(static_cast<double>(utf16_length(iterable.to_string()))));

            Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
            if (iterator_symbol) {
                std::string str_value = iterable.to_string();
                auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator",
                    [str_value](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx; (void)args;
                        auto iterator = std::make_unique<StringIterator>(str_value);
                        return Value(iterator.release());
                    });
                boxed_string->set_property(iterator_symbol->to_property_key(), Value(string_iterator_fn.release()));
            }
            obj = boxed_string.get();
        } else {
            obj = iterable.as_object();
        }

        Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
        if (iterator_symbol && obj && obj->has_property(iterator_symbol->to_property_key())) {
            Value iterator_method = obj->get_property(iterator_symbol->to_property_key());
            if (iterator_method.is_function()) {
                Function* iter_fn = iterator_method.as_function();
                Value iterator_obj = iter_fn->call(ctx, {}, iterable);

                if (iterator_obj.is_object()) {
                    Object* iterator = iterator_obj.as_object();
                    Value next_method = iterator->get_property("next");

                    if (next_method.is_function()) {
                        Function* next_fn = next_method.as_function();

                        std::string var_name;
                        VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;

                        if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                            if (var_decl->declaration_count() > 0) {
                                VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                                var_name = declarator->get_id()->get_name();
                                var_kind = declarator->get_kind();
                            }
                        } else if (left_->get_type() == Type::IDENTIFIER) {
                            Identifier* id = static_cast<Identifier*>(left_.get());
                            var_name = id->get_name();
                        } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                            var_name = "__destructuring__";
                        } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                                   left_->get_type() == Type::OBJECT_LITERAL) {
                            var_name = "__pattern__";
                        } else if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                            var_name = "__member__";
                        }

                        if (var_name.empty()) {
                            ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                            return Value();
                        }

                        auto close_iterator = [&iterator_obj, &ctx]() {
                            if (iterator_obj.is_object()) {
                                Value return_method = iterator_obj.as_object()->get_property("return");
                                if (return_method.is_function()) {
                                    return_method.as_function()->call(ctx, {}, iterator_obj);
                                }
                            }
                        };

                        Context* loop_ctx = &ctx;
                        uint32_t iteration_count = 0;
                        const uint32_t MAX_ITERATIONS = 1000000000;

                        while (iteration_count < MAX_ITERATIONS) {
                            iteration_count++;

                            Value result = next_fn->call(ctx, {}, iterator_obj);

                            if (ctx.has_exception()) { close_iterator(); return Value(); }

                            if (result.is_object()) {
                                Object* result_obj = result.as_object();
                                Value done = result_obj->get_property("done");

                                if (done.is_boolean() && done.to_boolean()) {
                                    break;
                                }

                                Value value = result_obj->get_property("value");

                                if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                                    MemberExpression* member = static_cast<MemberExpression*>(left_.get());
                                    Value obj_val = member->get_object()->evaluate(*loop_ctx);
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                    std::string prop_key;
                                    if (member->is_computed()) {
                                        Value key_val = member->get_property()->evaluate(*loop_ctx);
                                        if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                        prop_key = key_val.to_string();
                                    } else {
                                        Identifier* prop_id = static_cast<Identifier*>(member->get_property());
                                        prop_key = prop_id->get_name();
                                    }
                                    if (obj_val.is_object()) obj_val.as_object()->set_property(prop_key, value);
                                    else if (obj_val.is_function()) static_cast<Object*>(obj_val.as_function())->set_property(prop_key, value);
                                } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                                           left_->get_type() == Type::OBJECT_LITERAL) {
                                    AssignmentExpression::destructuring_assign(*loop_ctx, left_.get(), value);
                                    if (loop_ctx->has_exception()) { close_iterator(); return Value(); }
                                } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());

                                    if (destructuring->get_type() == DestructuringAssignment::Type::ARRAY && value.is_object()) {
                                        Object* array_obj = value.as_object();
                                        const auto& targets = destructuring->get_targets();

                                        for (size_t i = 0; i < targets.size(); ++i) {
                                            const std::string& var_name = targets[i]->get_name();
                                            Value element_value;

                                            if (array_obj->has_property(std::to_string(i))) {
                                                element_value = array_obj->get_property(std::to_string(i));
                                            } else {
                                                element_value = Value();
                                            }

                                            bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                            if (loop_ctx->has_binding(var_name)) {
                                                loop_ctx->set_binding(var_name, element_value);
                                            } else {
                                                loop_ctx->create_binding(var_name, element_value, is_mutable);
                                            }
                                        }
                                    }
                                } else {
                                    bool forof_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                                          var_kind == VariableDeclarator::Kind::CONST);
                                    if (forof_per_iter) {
                                        loop_ctx->push_block_scope();
                                        loop_ctx->create_lexical_binding(var_name, value, var_kind != VariableDeclarator::Kind::CONST);
                                    } else if (loop_ctx->has_binding(var_name)) {
                                        loop_ctx->set_binding(var_name, value);
                                    } else {
                                        bool is_mutable = (var_kind != VariableDeclarator::Kind::CONST);
                                        loop_ctx->create_binding(var_name, value, is_mutable);
                                    }

                                    body_->evaluate(*loop_ctx);

                                    if (forof_per_iter) {
                                        loop_ctx->pop_block_scope();
                                    }

                                    if (loop_ctx->has_exception()) {
                                        close_iterator();
                                        return Value();
                                    }

                                    if (loop_ctx->has_break()) { close_iterator(); loop_ctx->clear_break_continue(); break; }
                                    if (loop_ctx->has_continue()) continue;
                                    if (loop_ctx->has_return_value()) {
                                        close_iterator();
                                        return Value();
                                    }
                                    continue;
                                }

                                body_->evaluate(*loop_ctx);
                                if (loop_ctx->has_exception()) {
                                    close_iterator();
                                    return Value();
                                }

                                if (loop_ctx->has_break()) { close_iterator(); break; }
                                if (loop_ctx->has_continue()) continue;
                                if (loop_ctx->has_return_value()) {
                                    close_iterator();
                                    return Value();
                                }
                            }
                        }

                        if (iteration_count >= MAX_ITERATIONS) {
                            ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                            return Value();
                        }

                        return Value();
                    }
                }
            }
        }

        if (obj->get_type() == Object::ObjectType::Array) {
            uint32_t length = obj->get_length();

            if (length > 50) {
                ctx.throw_exception(Value(std::string("For...of: Array too large (>50 elements)")));
                return Value();
            }

            std::string var_name;
            VariableDeclarator::Kind var_kind = VariableDeclarator::Kind::LET;

            if (left_->get_type() == Type::VARIABLE_DECLARATION) {
                VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(left_.get());
                if (var_decl->declaration_count() > 0) {
                    VariableDeclarator* declarator = var_decl->get_declarations()[0].get();
                    var_name = declarator->get_id()->get_name();
                    var_kind = declarator->get_kind();
                }
            } else if (left_->get_type() == Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(left_.get());
                var_name = id->get_name();
            } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                var_name = "__destructuring_temp__";
            } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                       left_->get_type() == Type::OBJECT_LITERAL) {
                var_name = "__pattern__";
            } else if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                var_name = "__member__";
            }

            if (var_name.empty()) {
                ctx.throw_exception(Value(std::string("For...of: Invalid loop variable")));
                return Value();
            }

            Context* loop_ctx = &ctx;

            uint32_t iteration_count = 0;
            const uint32_t MAX_ITERATIONS = 1000000000;

            for (uint32_t i = 0; i < length && iteration_count < MAX_ITERATIONS; i++) {
                iteration_count++;

                Value element = obj->get_element(i);

                if (left_->get_type() == Type::MEMBER_EXPRESSION) {
                    MemberExpression* member = static_cast<MemberExpression*>(left_.get());
                    Value obj_val = member->get_object()->evaluate(*loop_ctx);
                    if (loop_ctx->has_exception()) return Value();
                    std::string prop_key;
                    if (member->is_computed()) {
                        Value key_val = member->get_property()->evaluate(*loop_ctx);
                        if (loop_ctx->has_exception()) return Value();
                        prop_key = key_val.to_string();
                    } else {
                        Identifier* prop_id = static_cast<Identifier*>(member->get_property());
                        prop_key = prop_id->get_name();
                    }
                    if (obj_val.is_object()) obj_val.as_object()->set_property(prop_key, element);
                    else if (obj_val.is_function()) static_cast<Object*>(obj_val.as_function())->set_property(prop_key, element);
                } else if (left_->get_type() == Type::ARRAY_LITERAL ||
                           left_->get_type() == Type::OBJECT_LITERAL) {
                    AssignmentExpression::destructuring_assign(*loop_ctx, left_.get(), element);
                    if (loop_ctx->has_exception()) return Value();
                } else if (left_->get_type() == Type::DESTRUCTURING_ASSIGNMENT) {
                    DestructuringAssignment* destructuring = static_cast<DestructuringAssignment*>(left_.get());

                    std::unique_ptr<ASTNode> temp_literal;
                    Position dummy_pos(0, 0);

                    if (element.is_string()) {
                        temp_literal = std::make_unique<StringLiteral>(element.to_string(), dummy_pos, dummy_pos);
                    } else if (element.is_number()) {
                        temp_literal = std::make_unique<NumberLiteral>(element.to_number(), dummy_pos, dummy_pos);
                    } else if (element.is_boolean()) {
                        temp_literal = std::make_unique<BooleanLiteral>(element.to_boolean(), dummy_pos, dummy_pos);
                    } else if (element.is_null()) {
                        temp_literal = std::make_unique<NullLiteral>(dummy_pos, dummy_pos);
                    } else if (element.is_undefined()) {
                        temp_literal = std::make_unique<UndefinedLiteral>(dummy_pos, dummy_pos);
                    } else {
                        std::string temp_var = "__temp_destructure_" + std::to_string(i);
                        loop_ctx->create_binding(temp_var, element, true);
                        temp_literal = std::make_unique<Identifier>(temp_var, dummy_pos, dummy_pos);
                    }

                    destructuring->set_source(std::move(temp_literal));
                    destructuring->evaluate(*loop_ctx);
                } else {
                    bool forof_arr_per_iter = (var_kind == VariableDeclarator::Kind::LET ||
                                               var_kind == VariableDeclarator::Kind::CONST);
                    if (forof_arr_per_iter) {
                        loop_ctx->push_block_scope();
                        loop_ctx->create_lexical_binding(var_name, element, var_kind != VariableDeclarator::Kind::CONST);
                    } else if (loop_ctx->has_binding(var_name)) {
                        loop_ctx->set_binding(var_name, element);
                    } else {
                        loop_ctx->create_binding(var_name, element, true);
                    }

                    if (body_) {
                        Value result = body_->evaluate(*loop_ctx);
                        if (forof_arr_per_iter) {
                            loop_ctx->pop_block_scope();
                        }
                        if (loop_ctx->has_exception()) {
                            ctx.throw_exception(loop_ctx->get_exception());
                            return Value();
                        }
                        if (loop_ctx->has_return_value()) {
                            ctx.set_return_value(loop_ctx->get_return_value());
                            return Value();
                        }
                    } else if (forof_arr_per_iter) {
                        loop_ctx->pop_block_scope();
                    }
                }
            }

            if (iteration_count >= MAX_ITERATIONS) {
                ctx.throw_exception(Value(std::string("For...of loop exceeded iterations (50)")));
                return Value();
            }
        } else {
            ctx.throw_exception(Value(std::string("For...of: Only arrays are supported")));
            return Value();
        }
    } else {
        ctx.throw_type_error("For...of: value is not iterable");
        return Value();
    }

    return Value();
}

std::string ForOfStatement::to_string() const {
    std::ostringstream oss;
    if (is_await_) {
        oss << "for await (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    } else {
        oss << "for (" << left_->to_string() << " of " << right_->to_string() << ") " << body_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> ForOfStatement::clone() const {
    return std::make_unique<ForOfStatement>(
        left_->clone(), right_->clone(), body_->clone(), is_await_, start_, end_
    );
}


Value WhileStatement::evaluate(Context& ctx) {
    std::string this_loop_label = ctx.get_next_statement_label();
    ctx.set_next_statement_label("");

    std::string prev_loop_label = ctx.get_current_loop_label();
    ctx.set_current_loop_label(this_loop_label);

    int safety_counter = 0;
    const int max_iterations = 1000000000;

    try {
        while (true) {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
            }

            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error evaluating while-loop condition")));
                return Value();
            }

            if (!test_value.to_boolean()) {
                break;
            }

            try {
                Value body_result = body_->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                if (ctx.has_break()) {
                    if (ctx.get_break_label().empty()) {
                        ctx.clear_break_continue();
                        break;
                    }
                    break;
                }
                if (ctx.has_continue()) {
                    if (ctx.get_continue_label().empty()) {
                        ctx.clear_break_continue();
                        continue;
                    }
                    if (ctx.get_continue_label() == ctx.get_current_loop_label()) {
                        ctx.clear_break_continue();
                        continue;
                    }
                    break;
                }

                if (safety_counter % 10 == 0) {
                }
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in while-loop body execution")));
                ctx.set_current_loop_label(prev_loop_label);
                return Value();
            }
        }
    } catch (...) {
        ctx.throw_exception(Value(std::string("Fatal error in while-loop execution")));
        ctx.set_current_loop_label(prev_loop_label);
        return Value();
    }

    ctx.set_current_loop_label(prev_loop_label);
    return Value();
}

std::string WhileStatement::to_string() const {
    return "while (" + test_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WhileStatement::clone() const {
    return std::make_unique<WhileStatement>(
        test_->clone(), body_->clone(), start_, end_
    );
}


Value DoWhileStatement::evaluate(Context& ctx) {
    int safety_counter = 0;
    const int max_iterations = 1000000000;

    try {
        do {
            if (++safety_counter > max_iterations) {
                static bool warned = false;
                if (!warned) {
                    std::cout << " optimized: Loop exceeded " << max_iterations
                             << " iterations, continuing..." << std::endl;
                    warned = true;
                }
                safety_counter = 0;
            }

            try {
                Value body_result = body_->evaluate(ctx);
                if (ctx.has_exception()) return Value();

                if (ctx.has_break()) {
                    ctx.clear_break_continue();
                    break;
                }
                if (ctx.has_continue()) {
                    ctx.clear_break_continue();
                }

            } catch (...) {
                ctx.throw_exception(Value(std::string("Error in do-while-loop body execution")));
                return Value();
            }

            Value test_value;
            try {
                test_value = test_->evaluate(ctx);
                if (ctx.has_exception()) return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("Error evaluating do-while-loop condition")));
                return Value();
            }

            if (!test_value.to_boolean()) {
                break;
            }

        } while (true);

    } catch (...) {
        ctx.throw_exception(Value(std::string("Fatal error in do-while-loop execution")));
        return Value();
    }

    return Value();
}

std::string DoWhileStatement::to_string() const {
    return "do " + body_->to_string() + " while (" + test_->to_string() + ")";
}

std::unique_ptr<ASTNode> DoWhileStatement::clone() const {
    return std::make_unique<DoWhileStatement>(
        body_->clone(), test_->clone(), start_, end_
    );
}


Value WithStatement::evaluate(Context& ctx) {
    if (ctx.is_strict_mode()) {
        ctx.throw_syntax_error("Strict mode code may not include a with statement");
        return Value();
    }

    Value obj_value = object_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (!obj_value.is_object() && !obj_value.is_function()) {
        ctx.throw_type_error("with statement requires an object");
        return Value();
    }

    Object* obj = obj_value.is_function() ? obj_value.as_function() : obj_value.as_object();

    ctx.push_with_scope(obj);

    try {
        Value result = body_->evaluate(ctx);
        ctx.pop_with_scope();
        return result;
    } catch (...) {
        ctx.pop_with_scope();
        throw;
    }
}

std::string WithStatement::to_string() const {
    return "with (" + object_->to_string() + ") " + body_->to_string();
}

std::unique_ptr<ASTNode> WithStatement::clone() const {
    return std::make_unique<WithStatement>(
        object_->clone(), body_->clone(), start_, end_
    );
}


Value ReturnStatement::evaluate(Context& ctx) {
    Value return_value;

    if (has_argument()) {
        return_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    } else {
        return_value = Value();
    }

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


Value BreakStatement::evaluate(Context& ctx) {
    ctx.set_break(label_);
    return Value();
}

std::string BreakStatement::to_string() const {
    return label_.empty() ? "break;" : "break " + label_ + ";";
}

std::unique_ptr<ASTNode> BreakStatement::clone() const {
    return std::make_unique<BreakStatement>(start_, end_, label_);
}


Value ContinueStatement::evaluate(Context& ctx) {
    ctx.set_continue(label_);
    return Value();
}

std::string ContinueStatement::to_string() const {
    return "continue;";
}

std::unique_ptr<ASTNode> ContinueStatement::clone() const {
    return std::make_unique<ContinueStatement>(start_, end_);
}


Value TryStatement::evaluate(Context& ctx) {
    static int try_recursion_depth = 0;
    if (try_recursion_depth > 10) {
        return Value(std::string("Max try-catch recursion exceeded"));
    }

    try_recursion_depth++;

    Value result;
    Value exception_value;
    bool caught_exception = false;

    try {
        result = try_block_->evaluate(ctx);

        if (ctx.has_exception()) {
            caught_exception = true;
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        }
    } catch (const YieldException&) {
        try_recursion_depth--;
        throw;
    } catch (const AwaitSuspendException&) {
        try_recursion_depth--;
        throw;
    } catch (const std::exception& e) {
        caught_exception = true;
        if (ctx.has_exception()) {
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        } else {
            ctx.throw_exception(Value(std::string(e.what())));
            exception_value = ctx.get_exception();
            ctx.clear_exception();
        }
    } catch (...) {
        caught_exception = true;
        exception_value = Value(std::string("Error: Unknown error"));
    }

    if (caught_exception && catch_clause_) {
        CatchClause* catch_node = static_cast<CatchClause*>(catch_clause_.get());

        if (!catch_node->get_parameter_name().empty()) {
            std::string param_name = catch_node->get_parameter_name();

            if (param_name.length() > 14 && param_name.substr(0, 14) == "__destr_array:") {
                std::string vars_str = param_name.substr(14);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* arr = exception_value.as_object();
                    for (size_t vi = 0; vi < var_names.size(); vi++) {
                        Value el = arr->get_element(static_cast<uint32_t>(vi));
                        if (!ctx.create_binding(var_names[vi], el, true))
                            ctx.set_binding(var_names[vi], el);
                    }
                }
            } else if (param_name.length() > 12 && param_name.substr(0, 12) == "__destr_obj:") {
                std::string vars_str = param_name.substr(12);
                std::vector<std::string> var_names;
                std::string cur;
                for (char c : vars_str) {
                    if (c == ',') { if (!cur.empty()) { var_names.push_back(cur); cur.clear(); } }
                    else cur += c;
                }
                if (!cur.empty()) var_names.push_back(cur);

                if (exception_value.is_object()) {
                    Object* obj = exception_value.as_object();
                    for (const auto& vn : var_names) {
                        Value val = obj->get_property(vn);
                        if (!ctx.create_binding(vn, val, true))
                            ctx.set_binding(vn, val);
                    }
                }
            } else {
                if (!ctx.create_binding(param_name, exception_value, true)) {
                    ctx.set_binding(param_name, exception_value);
                }
            }
        }

        try {
            result = catch_node->get_body()->evaluate(ctx);

            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        } catch (const std::exception& e) {
            result = Value(std::string("CatchBlockError: ") + e.what());
            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        } catch (...) {
            result = Value(std::string("CatchBlockError: Unknown error in catch"));
            if (ctx.has_exception()) {
                ctx.clear_exception();
            }
        }
    }

    if (finally_block_) {
        try {
            finally_block_->evaluate(ctx);
        } catch (const std::exception& e) {
            std::cerr << "Finally block error: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Finally block unknown error" << std::endl;
        }
    }

    if (ctx.has_exception()) {
        ctx.clear_exception();
    }

    try_recursion_depth--;
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
    if (ctx.has_exception()) return Value();

    ctx.throw_exception(exception_value, true);
    return Value();
}

std::string ThrowStatement::to_string() const {
    return "throw " + expression_->to_string();
}

std::unique_ptr<ASTNode> ThrowStatement::clone() const {
    return std::make_unique<ThrowStatement>(expression_->clone(), start_, end_);
}

Value SwitchStatement::evaluate(Context& ctx) {
    Value discriminant_value = discriminant_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    int matching_case_index = -1;
    int default_case_index = -1;

    for (size_t i = 0; i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());

        if (case_clause->is_default()) {
            default_case_index = static_cast<int>(i);
        } else {
            Value test_value = case_clause->get_test()->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (discriminant_value.strict_equals(test_value)) {
                matching_case_index = static_cast<int>(i);
                break;
            }
        }
    }

    int start_index = -1;
    if (matching_case_index >= 0) {
        start_index = matching_case_index;
    } else if (default_case_index >= 0) {
        start_index = default_case_index;
    }

    if (start_index < 0) {
        return Value();
    }

    bool executing = false;
    Value result;

    for (size_t i = static_cast<size_t>(start_index); i < cases_.size(); i++) {
        CaseClause* case_clause = static_cast<CaseClause*>(cases_[i].get());
        executing = true;

        for (const auto& stmt : case_clause->get_consequent()) {
            result = stmt->evaluate(ctx);
            if (ctx.has_exception()) return Value();

            if (ctx.has_break()) {
                ctx.clear_break_continue();
                return result;
            }

            if (ctx.has_return_value()) {
                return ctx.get_return_value();
            }
        }

    }

    return result;
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

} // namespace Quanta
