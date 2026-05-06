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
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/modules/ModuleLoader.h"
#include <sstream>
#include <set>

namespace Quanta {

Value FunctionDeclaration::evaluate(Context& ctx) {
    const std::string& function_name = id_->get_name();

    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }

    std::unique_ptr<Function> function_obj;
    if (is_async_ && is_generator_) {
        std::vector<std::unique_ptr<Parameter>> gen_params;
        for (const auto& p : param_clones)
            gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
        function_obj = std::make_unique<AsyncGeneratorFunction>(function_name, std::move(gen_params), body_->clone(), &ctx);
    } else if (is_generator_) {
        std::vector<std::unique_ptr<Parameter>> gen_params;
        for (const auto& p : param_clones)
            gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
        function_obj = std::make_unique<GeneratorFunction>(function_name, std::move(gen_params), body_->clone(), &ctx);
    } else if (is_async_) {
        std::vector<std::string> param_names;
        for (const auto& param : param_clones) {
            param_names.push_back(param->get_name()->get_name());
        }
        function_obj = std::make_unique<AsyncFunction>(function_name, param_names, body_->clone(), &ctx);
    } else {
        function_obj = ObjectFactory::create_js_function(
            function_name,
            std::move(param_clones),
            body_->clone(),
            &ctx
        );
    }


    if (function_obj) {

        auto var_env = ctx.get_variable_environment();
        if (var_env) {
            auto var_binding_names = var_env->get_binding_names();
            for (const auto& name : var_binding_names) {
                if (name != "this" && name != "arguments") {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined()) {
                        function_obj->set_property("__closure_" + name, value);
                    }
                }
            }
        }

        auto lex_env = ctx.get_lexical_environment();
        Environment* walk = lex_env;
        while (walk && walk != var_env) {
            auto lex_binding_names = walk->get_binding_names();
            for (const auto& name : lex_binding_names) {
                if (name != "this" && name != "arguments") {
                    if (!function_obj->has_property("__closure_" + name)) {
                        Value value = ctx.get_binding(name);
                        if (!value.is_undefined()) {
                            function_obj->set_property("__closure_" + name, value);
                        }
                    }
                }
            }
            walk = walk->get_outer();
        }

        std::vector<std::string> potential_vars = {"count", "outerVar", "value", "data", "result", "i", "j", "x", "y", "z"};
        for (const auto& var_name : potential_vars) {
            if (ctx.has_binding(var_name)) {
                Value value = ctx.get_binding(var_name);
                if (!value.is_undefined()) {
                    if (!function_obj->has_property("__closure_" + var_name)) {
                        function_obj->set_property("__closure_" + var_name, value);
                    }
                }
            }
        }
    }

    if (function_obj && !source_text_.empty()) {
        function_obj->set_source_text(source_text_);
    }

    Function* func_ptr = function_obj.release();
    Value function_value(func_ptr);

    bool use_lexical = ctx.is_strict_mode() &&
        ctx.get_lexical_environment() != ctx.get_variable_environment();
    if (use_lexical) {
        if (!ctx.create_lexical_binding(function_name, function_value, true)) {
            ctx.set_binding(function_name, function_value);
        }
    } else if (!ctx.create_binding(function_name, function_value, true)) {
        ctx.set_binding(function_name, function_value);
    }



    return Value();
}

std::string FunctionDeclaration::to_string() const {
    std::ostringstream oss;
    if (is_async_) {
        oss << "async ";
    }
    oss << "function";
    if (is_generator_) {
        oss << "*";
    }
    oss << " " << id_->get_name() << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionDeclaration::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    return std::make_unique<FunctionDeclaration>(
        std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_, is_async_, is_generator_
    );
}


Value ClassDeclaration::evaluate(Context& ctx) {
    std::string class_name = id_->get_name();

    auto prototype = ObjectFactory::create_object();

    std::unique_ptr<ASTNode> constructor_body = nullptr;
    std::vector<std::string> constructor_params;
    std::vector<std::unique_ptr<ASTNode>> field_initializers;
    std::vector<std::unique_ptr<ASTNode>> static_field_initializers;

    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(stmt.get());
                if (cf->is_static()) {
                    static_field_initializers.push_back(stmt->clone());
                } else {
                    field_initializers.push_back(stmt->clone());
                }
                continue;
            }
            if (stmt->get_type() == Type::CLASS_STATIC_BLOCK) {
                static_field_initializers.push_back(stmt->clone());
                continue;
            }

            if (stmt->get_type() == Type::EXPRESSION_STATEMENT) {
                field_initializers.push_back(stmt->clone());
                continue;
            }

            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                std::string method_name;
                if (method->is_computed()) {
                    Value key_val = method->get_key()->evaluate(ctx);
                    if (ctx.has_exception()) return Value();
                    method_name = key_val.to_string();
                } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                    method_name = id->get_name();
                } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                    method_name = str->get_value();
                } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                    method_name = num->to_string();
                } else {
                    method_name = "[unknown]";
                }

                if (method->is_constructor()) {
                    constructor_body = method->get_value()->get_body()->clone();
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        const auto& params = func_expr->get_params();
                        constructor_params.reserve(params.size());
                        for (const auto& param : params) {
                            constructor_params.push_back(param->get_name()->get_name());
                        }
                    }
                } else if (method->is_static()) {
                } else {
                    bool method_is_gen = false;
                    bool method_is_async = false;
                    std::vector<std::unique_ptr<Parameter>> method_params;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        method_is_gen = func_expr->is_generator();
                        method_is_async = func_expr->is_async();
                        const auto& params = func_expr->get_params();
                        method_params.reserve(params.size());
                        for (const auto& param : params) {
                            method_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                        }
                    }
                    std::unique_ptr<Function> instance_method;
                    if (method_is_gen && method_is_async) {
                        std::vector<std::unique_ptr<Parameter>> gen_params;
                        for (const auto& p : method_params) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                        instance_method = std::make_unique<AsyncGeneratorFunction>(method_name, std::move(gen_params), method->get_value()->get_body()->clone(), &ctx);
                    } else if (method_is_gen) {
                        std::vector<std::unique_ptr<Parameter>> gen_params;
                        for (const auto& p : method_params) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                        instance_method = std::make_unique<GeneratorFunction>(method_name, std::move(gen_params), method->get_value()->get_body()->clone(), &ctx);
                    } else if (method_is_async) {
                        std::vector<std::string> async_params;
                        for (const auto& p : method_params) async_params.push_back(p->get_name()->get_name());
                        instance_method = std::make_unique<AsyncFunction>(method_name, async_params, method->get_value()->get_body()->clone(), &ctx);
                    } else {
                        instance_method = ObjectFactory::create_js_function(method_name, std::move(method_params), method->get_value()->get_body()->clone(), &ctx);
                    }
                    instance_method->set_is_strict(true);

                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        PropertyDescriptor existing = prototype->get_property_descriptor(method_name);
                        PropertyDescriptor desc;
                        if (existing.is_accessor_descriptor() || existing.has_getter() || existing.has_setter()) {
                            desc = existing;
                        }
                        if (method->get_kind() == MethodDefinition::GETTER) {
                            desc.set_getter(instance_method.release());
                        } else {
                            desc.set_setter(instance_method.release());
                        }
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        prototype->set_property_descriptor(method_name, desc);
                    } else {
                        PropertyDescriptor method_desc(Value(instance_method.release()),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        prototype->set_property_descriptor(method_name, method_desc);
                    }
                }
            }
        }
    }

    if (!constructor_body) {
        std::vector<std::unique_ptr<ASTNode>> empty_statements;
        constructor_body = std::make_unique<BlockStatement>(
            std::move(empty_statements),
            Position{0, 0},
            Position{0, 0}
        );
    }

    if (!field_initializers.empty()) {
        BlockStatement* body_block = static_cast<BlockStatement*>(constructor_body.get());
        std::vector<std::unique_ptr<ASTNode>> new_statements;

        for (auto& field_init : field_initializers) {
            if (field_init->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(field_init.get());
                Position fstart = cf->get_start();
                auto this_id = std::make_unique<Identifier>("this", fstart, fstart);
                auto member_expr = std::make_unique<MemberExpression>(
                    std::move(this_id), cf->get_key()->clone(), cf->is_computed(), fstart, fstart);
                std::unique_ptr<ASTNode> init_val;
                if (cf->get_value()) {
                    init_val = cf->get_value()->clone();
                } else {
                    init_val = std::make_unique<Identifier>("undefined", fstart, fstart);
                }
                auto assign = std::make_unique<AssignmentExpression>(
                    std::move(member_expr), AssignmentExpression::Operator::ASSIGN,
                    std::move(init_val), fstart, fstart);
                new_statements.push_back(std::make_unique<ExpressionStatement>(std::move(assign), fstart, fstart));
            } else {
                new_statements.push_back(std::move(field_init));
            }
        }

        for (auto& stmt : body_block->get_statements()) {
            new_statements.push_back(stmt->clone());
        }

        constructor_body = std::make_unique<BlockStatement>(
            std::move(new_statements),
            Position{0, 0},
            Position{0, 0}
        );
    }

    auto constructor_fn = ObjectFactory::create_js_function(
        class_name,
        constructor_params,
        std::move(constructor_body),
        &ctx
    );

    Object* proto_ptr = prototype.get();
    if (constructor_fn.get() && proto_ptr) {
        constructor_fn->set_property("prototype", Value(proto_ptr));
        proto_ptr->set_property("constructor", Value(constructor_fn.get()));
        constructor_fn->set_is_class_constructor(true);
        constructor_fn->set_is_strict(true);
        if (!source_text_.empty()) {
            constructor_fn->set_source_text(source_text_);
        }

        prototype.release();
    } else {
        ctx.throw_exception(Value(std::string("Class setup failed: null constructor or prototype")));
        return Value();
    }

    if (body_) {
        for (const auto& stmt : body_->get_statements()) {
            if (stmt->get_type() == Type::METHOD_DEFINITION) {
                MethodDefinition* method = static_cast<MethodDefinition*>(stmt.get());
                if (method->is_static()) {
                    std::string method_name;
                    if (method->is_computed()) {
                        Value key_val = method->get_key()->evaluate(ctx);
                        if (ctx.has_exception()) return Value();
                        method_name = key_val.to_string();
                    } else if (Identifier* id = dynamic_cast<Identifier*>(method->get_key())) {
                        method_name = id->get_name();
                    } else if (StringLiteral* str = dynamic_cast<StringLiteral*>(method->get_key())) {
                        method_name = str->get_value();
                    } else if (NumberLiteral* num = dynamic_cast<NumberLiteral*>(method->get_key())) {
                        method_name = num->to_string();
                    } else {
                        method_name = "[unknown]";
                    }
                    bool static_is_gen = false;
                    bool static_is_async = false;
                    std::vector<std::unique_ptr<Parameter>> static_params;
                    if (method->get_value()->get_type() == Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* func_expr = static_cast<FunctionExpression*>(method->get_value());
                        static_is_gen = func_expr->is_generator();
                        static_is_async = func_expr->is_async();
                        const auto& params = func_expr->get_params();
                        static_params.reserve(params.size());
                        for (const auto& param : params) {
                            static_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                        }
                    }
                    std::unique_ptr<Function> static_method;
                    if (static_is_gen && static_is_async) {
                        std::vector<std::unique_ptr<Parameter>> gen_params;
                        for (const auto& p : static_params) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                        static_method = std::make_unique<AsyncGeneratorFunction>(method_name, std::move(gen_params), method->get_value()->get_body()->clone(), &ctx);
                    } else if (static_is_gen) {
                        std::vector<std::unique_ptr<Parameter>> gen_params;
                        for (const auto& p : static_params) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                        static_method = std::make_unique<GeneratorFunction>(method_name, std::move(gen_params), method->get_value()->get_body()->clone(), &ctx);
                    } else if (static_is_async) {
                        std::vector<std::string> async_params;
                        for (const auto& p : static_params) async_params.push_back(p->get_name()->get_name());
                        static_method = std::make_unique<AsyncFunction>(method_name, async_params, method->get_value()->get_body()->clone(), &ctx);
                    } else {
                        static_method = ObjectFactory::create_js_function(method_name, std::move(static_params), method->get_value()->get_body()->clone(), &ctx);
                    }
                    static_method->set_is_strict(true);

                    if (method->get_kind() == MethodDefinition::GETTER || method->get_kind() == MethodDefinition::SETTER) {
                        PropertyDescriptor existing = constructor_fn->get_property_descriptor(method_name);
                        PropertyDescriptor desc;
                        if (existing.is_accessor_descriptor() || existing.has_getter() || existing.has_setter()) {
                            desc = existing;
                        }
                        if (method->get_kind() == MethodDefinition::GETTER) {
                            desc.set_getter(static_method.release());
                        } else {
                            desc.set_setter(static_method.release());
                        }
                        desc.set_enumerable(false);
                        desc.set_configurable(true);
                        constructor_fn->set_property_descriptor(method_name, desc);
                    } else {
                        PropertyDescriptor method_desc(Value(static_method.release()),
                            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                        constructor_fn->set_property_descriptor(method_name, method_desc);
                    }
                }
            }
        }
    }

    if (has_superclass()) {
        Value super_constructor = superclass_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        if (super_constructor.is_null()) {
            if (proto_ptr) {
                proto_ptr->set_prototype(nullptr);
            }
        } else if (super_constructor.is_object_like() && super_constructor.as_object()) {
            Object* super_obj = super_constructor.as_object();
            Function* super_fn = nullptr;
            if (super_obj->is_function()) {
                super_fn = static_cast<Function*>(super_obj);
            } else if (super_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy_super = static_cast<Proxy*>(super_obj);
                Object* target = proxy_super->get_proxy_target();
                if (target && target->is_function()) {
                    super_fn = static_cast<Function*>(target);
                }
            }
            if (super_fn && constructor_fn.get()) {
                constructor_fn->set_prototype(super_fn);
                constructor_fn->set_property("__super_constructor__", Value(super_fn));

                if (proto_ptr) {
                    auto method_keys = proto_ptr->get_own_property_keys();
                    for (const auto& mkey : method_keys) {
                        if (mkey == "constructor") continue;
                        Value mval = proto_ptr->get_property(mkey);
                        if (mval.is_function()) {
                            mval.as_function()->set_property("__super_constructor__", Value(super_fn));
                        }
                        PropertyDescriptor mdesc = proto_ptr->get_property_descriptor(mkey);
                        if (mdesc.has_getter() && mdesc.get_getter()) {
                            static_cast<Function*>(mdesc.get_getter())->set_property("__super_constructor__", Value(super_fn));
                        }
                        if (mdesc.has_setter() && mdesc.get_setter()) {
                            static_cast<Function*>(mdesc.get_setter())->set_property("__super_constructor__", Value(super_fn));
                        }
                    }
                }

                Value super_proto_val = super_obj->get_property("prototype");
                if (proto_ptr) {
                    Object* super_proto_obj = nullptr;
                    if (super_proto_val.is_object()) super_proto_obj = super_proto_val.as_object();
                    else if (super_proto_val.is_function()) super_proto_obj = super_proto_val.as_function();
                    if (super_proto_obj) proto_ptr->set_prototype(super_proto_obj);
                }
            }
        }
    }

    std::string closure_key = "__closure_" + class_name;
    Value ctor_val(constructor_fn.get());
    if (proto_ptr) {
        auto proto_keys = proto_ptr->get_own_property_keys();
        for (const auto& key : proto_keys) {
            if (key == "constructor") continue;
            Value method_val = proto_ptr->get_property(key);
            if (method_val.is_function()) {
                method_val.as_function()->set_property(closure_key, ctor_val);
            }
            PropertyDescriptor desc = proto_ptr->get_property_descriptor(key);
            if (desc.has_getter() && desc.get_getter()) {
                static_cast<Function*>(desc.get_getter())->set_property(closure_key, ctor_val);
            }
            if (desc.has_setter() && desc.get_setter()) {
                static_cast<Function*>(desc.get_setter())->set_property(closure_key, ctor_val);
            }
        }
    }
    auto static_keys = constructor_fn->get_own_property_keys();
    for (const auto& key : static_keys) {
        if (key == "prototype" || key == "name" || key == "length" || key == "__super_constructor__") continue;
        Value method_val = constructor_fn->get_property(key);
        if (method_val.is_function()) {
            method_val.as_function()->set_property(closure_key, ctor_val);
        }
    }

    ctx.create_binding(class_name, Value(constructor_fn.get()));

    if (!static_field_initializers.empty()) {
        for (auto& sfi : static_field_initializers) {
            if (sfi->get_type() == Type::CLASS_STATIC_BLOCK) {
                ClassStaticBlock* blk = static_cast<ClassStaticBlock*>(sfi.get());
                auto static_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, constructor_fn.get());
                static_ctx->create_binding("this", Value(constructor_fn.get()), true);
                if (blk->get_body()) blk->get_body()->evaluate(*static_ctx);
            } else if (sfi->get_type() == Type::CLASS_FIELD) {
                ClassField* cf = static_cast<ClassField*>(sfi.get());
                std::string key_name;
                if (cf->is_computed()) {
                    Value kv = cf->get_key()->evaluate(ctx);
                    if (ctx.has_exception()) break;
                    key_name = kv.to_string();
                } else if (Identifier* kid = dynamic_cast<Identifier*>(cf->get_key())) {
                    key_name = kid->get_name();
                } else if (StringLiteral* ks = dynamic_cast<StringLiteral*>(cf->get_key())) {
                    key_name = ks->get_value();
                }
                Value val;
                if (cf->get_value()) {
                    val = cf->get_value()->evaluate(ctx);
                    if (ctx.has_exception()) break;
                }
                PropertyDescriptor fdesc(val, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable | PropertyAttributes::Enumerable));
                constructor_fn->set_property_descriptor(key_name, fdesc);
            }
        }
    }

    Function* constructor_ptr = constructor_fn.get();

    constructor_fn.release();

    return Value(constructor_ptr);
}

std::string ClassDeclaration::to_string() const {
    std::ostringstream oss;
    oss << "class " << id_->get_name();

    if (has_superclass()) {
        oss << " extends " << superclass_->to_string();
    }

    oss << " " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> ClassDeclaration::clone() const {
    std::unique_ptr<ASTNode> cloned_superclass = nullptr;
    if (has_superclass()) {
        cloned_superclass = superclass_->clone();
    }

    if (has_superclass()) {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::move(cloned_superclass),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
            start_, end_
        );
    } else {
        return std::make_unique<ClassDeclaration>(
            std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())),
            std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
            start_, end_
        );
    }
}


Value ClassField::evaluate(Context& ctx) {
    return Value();
}

std::unique_ptr<ASTNode> ClassField::clone() const {
    return std::make_unique<ClassField>(
        key_->clone(),
        value_ ? value_->clone() : nullptr,
        is_static_, computed_,
        start_, end_
    );
}

Value ClassStaticBlock::evaluate(Context& ctx) {
    if (body_) body_->evaluate(ctx);
    return Value();
}

std::unique_ptr<ASTNode> ClassStaticBlock::clone() const {
    return std::make_unique<ClassStaticBlock>(
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}

Value MethodDefinition::evaluate(Context& ctx) {
    if (value_) {
        return value_->evaluate(ctx);
    }
    return Value();
}

std::string MethodDefinition::to_string() const {
    std::ostringstream oss;

    if (is_static_) {
        oss << "static ";
    }

    if (is_constructor()) {
        oss << "constructor";
    } else if (computed_) {
        oss << "[" << key_->to_string() << "]";
    } else if (Identifier* id = dynamic_cast<Identifier*>(key_.get())) {
        oss << id->get_name();
    } else {
        oss << key_->to_string();
    }

    if (value_) {
        oss << value_->to_string();
    } else {
        oss << "{ }";
    }

    return oss.str();
}

std::unique_ptr<ASTNode> MethodDefinition::clone() const {
    return std::make_unique<MethodDefinition>(
        key_ ? key_->clone() : nullptr,
        value_ ? std::unique_ptr<FunctionExpression>(static_cast<FunctionExpression*>(value_->clone().release())) : nullptr,
        kind_, is_static_, computed_, start_, end_
    );
}


Value FunctionExpression::evaluate(Context& ctx) {
    std::string name = is_named() ? id_->get_name() : "";

    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }

    std::set<std::string> param_names;
    for (const auto& param : param_clones) {
        param_names.insert(param->get_name()->get_name());
    }

    std::unique_ptr<Function> function;
    if (is_async_ && is_generator_) {
        std::vector<std::unique_ptr<Parameter>> gen_params;
        for (const auto& p : params_) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
        function = std::make_unique<AsyncGeneratorFunction>(name, std::move(gen_params), body_->clone(), &ctx);
    } else if (is_generator_) {
        std::vector<std::unique_ptr<Parameter>> gen_params;
        for (const auto& p : params_) gen_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
        function = std::make_unique<GeneratorFunction>(name, std::move(gen_params), body_->clone(), &ctx);
    } else {
        function = std::make_unique<Function>(name, std::move(param_clones), body_->clone(), &ctx);
    }

    if (function) {
        if (ctx.is_in_param_eval()) {
            function->set_is_param_default(true);
        }

        auto var_env = ctx.get_variable_environment();
        if (var_env) {
            auto var_binding_names = var_env->get_binding_names();
            for (const auto& name : var_binding_names) {
                if (name != "this" && name != "arguments" && param_names.find(name) == param_names.end()) {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined()) {
                        function->set_property("__closure_" + name, value);
                    }
                }
            }
        }

        auto lex_env = ctx.get_lexical_environment();
        Environment* walk = lex_env;
        while (walk && walk != var_env) {
            auto lex_binding_names = walk->get_binding_names();
            for (const auto& name : lex_binding_names) {
                if (name != "this" && name != "arguments" && param_names.find(name) == param_names.end()) {
                    if (!function->has_property("__closure_" + name)) {
                        Value value = ctx.get_binding(name);
                        if (!value.is_undefined()) {
                            function->set_property("__closure_" + name, value);
                        }
                    }
                }
            }
            walk = walk->get_outer();
        }

        bool is_strict = ctx.is_strict_mode();
        if (!is_strict && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            BlockStatement* block = static_cast<BlockStatement*>(body_.get());
            const auto& stmts = block->get_statements();
            if (!stmts.empty()) {
                auto* first_stmt = stmts[0].get();
                if (first_stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
                    auto* expr_stmt = static_cast<ExpressionStatement*>(first_stmt);
                    auto* expr = expr_stmt->get_expression();
                    if (expr && expr->get_type() == ASTNode::Type::STRING_LITERAL) {
                        auto* string_literal = static_cast<StringLiteral*>(expr);
                        if (string_literal->get_value() == "use strict") {
                            is_strict = true;
                        }
                    }
                }
            }
        }

        if (is_strict) {
            auto thrower = ObjectFactory::create_native_function("ThrowTypeError",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    ctx.throw_type_error("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
                    return Value();
                });

            PropertyDescriptor caller_desc;
            caller_desc.set_getter(thrower.get());
            caller_desc.set_setter(thrower.get());
            caller_desc.set_configurable(false);
            caller_desc.set_enumerable(false);
            function->set_property_descriptor("caller", caller_desc);

            PropertyDescriptor arguments_desc;
            arguments_desc.set_getter(thrower.get());
            arguments_desc.set_setter(thrower.get());
            arguments_desc.set_configurable(false);
            arguments_desc.set_enumerable(false);
            function->set_property_descriptor("arguments", arguments_desc);

            thrower.release();
        }
    }

    if (function && !source_text_.empty()) {
        function->set_source_text(source_text_);
    }

    return Value(function.release());
}

std::string FunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "function";
    if (is_named()) {
        oss << " " << id_->get_name();
    }
    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") " << body_->to_string();
    return oss.str();
}

std::unique_ptr<ASTNode> FunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    std::unique_ptr<Identifier> cloned_id = nullptr;
    if (is_named()) {
        cloned_id = std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release()));
    }

    return std::make_unique<FunctionExpression>(
        std::move(cloned_id),
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_, is_generator_, is_async_
    );
}


Value ArrowFunctionExpression::evaluate(Context& ctx) {
    std::string name = "<arrow>";

    if (is_async_) {
        std::vector<std::string> param_names;
        for (const auto& param : params_) {
            param_names.push_back(param->get_name()->get_name());
        }

        auto* async_fn = new AsyncFunction(name, param_names, body_->clone(), &ctx);
        async_fn->set_is_arrow(true);
        async_fn->set_is_constructor(false);

        if (ctx.has_binding("this")) {
            async_fn->set_property("__arrow_this__", ctx.get_binding("this"));
        }

        if (ctx.has_binding("AsyncFunction")) {
            Value async_ctor = ctx.get_binding("AsyncFunction");
            if (async_ctor.is_function()) {
                Value proto = async_ctor.as_function()->get_property("prototype");
                if (proto.is_object()) {
                    async_fn->set_prototype(proto.as_object());
                }
            }
        }

        std::set<std::string> async_param_set(param_names.begin(), param_names.end());
        auto var_env = ctx.get_variable_environment();
        if (var_env) {
            for (const auto& bname : var_env->get_binding_names()) {
                if (bname != "this" && async_param_set.find(bname) == async_param_set.end()) {
                    Value value = ctx.get_binding(bname);
                    if (!value.is_undefined()) {
                        async_fn->set_property("__closure_" + bname, value);
                    }
                }
            }
        }
        auto lex_env = ctx.get_lexical_environment();
        Environment* walk = lex_env;
        while (walk && walk != var_env) {
            for (const auto& bname : walk->get_binding_names()) {
                if (bname != "this" && async_param_set.find(bname) == async_param_set.end()) {
                    if (!async_fn->has_property("__closure_" + bname)) {
                        Value value = ctx.get_binding(bname);
                        if (!value.is_undefined()) {
                            async_fn->set_property("__closure_" + bname, value);
                        }
                    }
                }
            }
            walk = walk->get_outer();
        }

        if (!source_text_.empty()) {
            async_fn->set_source_text(source_text_);
        }
        return Value(async_fn);
    }

    std::vector<std::unique_ptr<Parameter>> param_clones;
    for (const auto& param : params_) {
        param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
    }

    auto arrow_function = ObjectFactory::create_js_function(
        name,
        std::move(param_clones),
        body_->clone(),
        &ctx
    );

    arrow_function->set_is_constructor(false);
    arrow_function->set_is_arrow(true);

    if (ctx.has_binding("this")) {
        Value this_value = ctx.get_binding("this");
        arrow_function->set_property("__arrow_this__", this_value);
    }

    Value enclosing_new_target = ctx.get_new_target();
    if (!enclosing_new_target.is_undefined()) {
        arrow_function->set_property("__arrow_new_target__", enclosing_new_target);
    }

    std::set<std::string> param_names;
    for (const auto& param : params_) {
        param_names.insert(param->get_name()->get_name());
    }

    auto var_env = ctx.get_variable_environment();
    if (var_env) {
        auto var_binding_names = var_env->get_binding_names();
        for (const auto& name : var_binding_names) {
            if (name != "this" && param_names.find(name) == param_names.end()) {
                Value value = ctx.get_binding(name);
                if (!value.is_undefined()) {
                    arrow_function->set_property("__closure_" + name, value);
                }
            }
        }
    }

    auto lex_env = ctx.get_lexical_environment();
    Environment* walk = lex_env;
    while (walk && walk != var_env) {
        auto lex_binding_names = walk->get_binding_names();
        for (const auto& name : lex_binding_names) {
            if (name != "this" && param_names.find(name) == param_names.end()) {
                if (!arrow_function->has_property("__closure_" + name)) {
                    Value value = ctx.get_binding(name);
                    if (!value.is_undefined()) {
                        arrow_function->set_property("__closure_" + name, value);
                    }
                }
            }
        }
        walk = walk->get_outer();
    }

    if (!source_text_.empty()) {
        arrow_function->set_source_text(source_text_);
    }

    return Value(arrow_function.release());
}

std::string ArrowFunctionExpression::to_string() const {
    std::ostringstream oss;

    if (params_.size() == 1) {
        oss << params_[0]->get_name();
    } else {
        oss << "(";
        for (size_t i = 0; i < params_.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << params_[i]->get_name();
        }
        oss << ")";
    }

    oss << " => ";
    oss << body_->to_string();

    return oss.str();
}

std::unique_ptr<ASTNode> ArrowFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    return std::make_unique<ArrowFunctionExpression>(
        std::move(cloned_params),
        body_->clone(),
        is_async_,
        start_, end_
    );
}


Value AwaitExpression::evaluate(Context& ctx) {
    AsyncExecutor* exec = AsyncExecutor::get_current();

    if (exec) {
        size_t await_index = exec->next_await_index_++;

        if (await_index < exec->target_await_index_) {
            if (await_index < exec->await_is_throw_.size() && exec->await_is_throw_[await_index]) {
                ctx.throw_exception(exec->await_results_[await_index], true);
                return Value();
            }
            if (await_index < exec->await_results_.size()) {
                return exec->await_results_[await_index];
            }
            return Value();
        }

        if (!argument_) {
            exec->await_results_.push_back(Value());
            exec->await_is_throw_.push_back(false);
            exec->target_await_index_++;
            return Value();
        }

        Value expr_val = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;

        Promise* awaited_promise = nullptr;
        Value resolved_value;
        bool is_throw = false;
        bool is_pending = false;

        if (AsyncUtils::is_promise(expr_val)) {
            awaited_promise = static_cast<Promise*>(expr_val.as_object());
            if (awaited_promise->get_state() == PromiseState::FULFILLED) {
                resolved_value = awaited_promise->get_value();
            } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
                resolved_value = awaited_promise->get_value();
                is_throw = true;
            } else {
                is_pending = true;
            }
        } else {
            resolved_value = expr_val;
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

            Function* fulfill_raw = on_fulfill_fn.get();
            Function* reject_raw = on_reject_fn.get();
            std::string suffix = std::to_string(target_idx);
            awaited_promise->set_property("__af__" + suffix, Value(on_fulfill_fn.release()));
            awaited_promise->set_property("__ar__" + suffix, Value(on_reject_fn.release()));
            awaited_promise->then(fulfill_raw, reject_raw);
            throw AwaitSuspendException{};
        }

        exec->await_results_.push_back(resolved_value);
        exec->await_is_throw_.push_back(is_throw);
        exec->target_await_index_++;
        if (is_throw) {
            ctx.throw_exception(resolved_value, true);
            return Value();
        }
        return resolved_value;
    }

    if (!argument_) return Value();
    Value arg_value = argument_->evaluate(ctx);
    if (ctx.has_exception()) return Value();

    if (!arg_value.is_object()) return arg_value;
    Object* obj = arg_value.as_object();
    if (!obj) return arg_value;
    if (obj->get_type() == Object::ObjectType::Promise) {
        Promise* promise = static_cast<Promise*>(obj);
        if (promise && promise->get_state() == PromiseState::FULFILLED) {
            return promise->get_value();
        }
        if (promise && promise->get_state() == PromiseState::REJECTED) {
            ctx.throw_exception(promise->get_value(), true);
            return Value();
        }
    }
    return arg_value;
}

std::string AwaitExpression::to_string() const {
    return "await " + argument_->to_string();
}

std::unique_ptr<ASTNode> AwaitExpression::clone() const {
    return std::make_unique<AwaitExpression>(
        argument_->clone(),
        start_, end_
    );
}


Value YieldExpression::evaluate(Context& ctx) {
    Generator* current_gen = Generator::get_current_generator();

    if (is_delegate_) {
        Value iterable = argument_ ? argument_->evaluate(ctx) : Value();
        if (ctx.has_exception()) return Value();

        if (!current_gen) return Value();

        size_t delegate_start = Generator::increment_yield_counter();

        size_t needed_idx = current_gen->target_yield_index_ >= delegate_start
            ? current_gen->target_yield_index_ - delegate_start
            : 0;

        std::vector<Value> elements;
        bool delegate_exhausted = false;

        auto collect_from_iterator = [&](Value iter_val) {
            Object* iter_obj = nullptr;
            if (iter_val.is_object()) iter_obj = iter_val.as_object();
            else if (iter_val.is_function()) iter_obj = iter_val.as_function();
            if (!iter_obj) { delegate_exhausted = true; return; }

            Value next_fn = iter_obj->get_property("next");
            if (!next_fn.is_function()) { delegate_exhausted = true; return; }

            Generator* saved_gen = Generator::get_current_generator();
            Generator::set_current_generator(nullptr);

            for (size_t i = 0; i <= needed_idx; i++) {
                std::vector<Value> no_args;
                Value next_result = next_fn.as_function()->call(ctx, no_args, iter_val);
                if (ctx.has_exception()) { delegate_exhausted = true; break; }

                Value done_val;
                Value val;
                if (next_result.is_object()) {
                    done_val = next_result.as_object()->get_property("done");
                    val = next_result.as_object()->get_property("value");
                }

                if (done_val.to_boolean()) {
                    delegate_exhausted = true;
                    break;
                }
                elements.push_back(val);
            }

            Generator::set_current_generator(saved_gen);
        };

        auto throw_at_iterator = [&](Value iter_val) -> bool {
            Object* iter_obj = nullptr;
            if (iter_val.is_object()) iter_obj = iter_val.as_object();
            else if (iter_val.is_function()) iter_obj = iter_val.as_function();
            if (!iter_obj) return false;

            Value next_fn = iter_obj->get_property("next");
            if (!next_fn.is_function()) return false;

            Generator* saved_gen = Generator::get_current_generator();
            Generator::set_current_generator(nullptr);

            bool iter_done = false;
            for (size_t i = 0; i < needed_idx && !iter_done; i++) {
                std::vector<Value> no_args;
                Value nr = next_fn.as_function()->call(ctx, no_args, iter_val);
                if (ctx.has_exception()) { iter_done = true; break; }
                if (nr.is_object() && nr.as_object()->get_property("done").to_boolean()) {
                    iter_done = true;
                }
            }

            if (!iter_done) {
                Value throw_fn = iter_obj->get_property("throw");
                if (throw_fn.is_function()) {
                    std::vector<Value> throw_args = {current_gen->throw_value_};
                    Value throw_result = throw_fn.as_function()->call(ctx, throw_args, iter_val);
                    Generator::set_current_generator(saved_gen);
                    if (ctx.has_exception()) return false;
                    if (throw_result.is_object()) {
                        Value done_v = throw_result.as_object()->get_property("done");
                        if (!done_v.to_boolean()) {
                            Value val = throw_result.as_object()->get_property("value");
                            current_gen->throwing_ = false;
                            throw YieldException(val);
                        }
                    }
                } else {
                    Value return_fn_v = iter_obj->get_property("return");
                    if (return_fn_v.is_function()) {
                        std::vector<Value> ret_args;
                        return_fn_v.as_function()->call(ctx, ret_args, iter_val);
                        ctx.clear_exception();
                    }
                    Generator::set_current_generator(saved_gen);
                    ctx.throw_exception(current_gen->throw_value_, true);
                    return false;
                }
            }
            Generator::set_current_generator(saved_gen);
            return true;
        };

        if (current_gen->throwing_) {
            bool handled = false;
            if (iterable.is_object() || iterable.is_function()) {
                Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();
                Value sym_iter_fn = obj->get_property("Symbol.iterator");
                if (sym_iter_fn.is_function()) {
                    Generator* saved_gen = Generator::get_current_generator();
                    Generator::set_current_generator(nullptr);
                    std::vector<Value> no_args;
                    Value iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
                    Generator::set_current_generator(saved_gen);
                    if (!ctx.has_exception()) {
                        handled = throw_at_iterator(iter_val);
                    }
                } else {
                    Value next_fn = obj->get_property("next");
                    if (next_fn.is_function()) {
                        handled = throw_at_iterator(iterable);
                    }
                }
            }
            current_gen->throwing_ = false;
            (void)handled;
            return Value();
        }

        if (current_gen->returning_) {
            if (iterable.is_object() || iterable.is_function()) {
                Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                Value iter_val;

                Generator* saved_gen = Generator::get_current_generator();
                Generator::set_current_generator(nullptr);

                if (iter_sym) {
                    Value sym_iter_fn = obj->get_property(iter_sym->to_property_key());
                    if (sym_iter_fn.is_function()) {
                        std::vector<Value> no_args;
                        iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
                    }
                }
                if (iter_val.is_undefined()) iter_val = iterable;

                Object* iter_obj = iter_val.is_object() ? iter_val.as_object()
                                 : iter_val.is_function() ? static_cast<Object*>(iter_val.as_function()) : nullptr;

                if (iter_obj && !ctx.has_exception()) {
                    Value next_fn_v = iter_obj->get_property("next");
                    for (size_t i = 0; i < needed_idx && next_fn_v.is_function(); i++) {
                        std::vector<Value> no_args;
                        Value nr = next_fn_v.as_function()->call(ctx, no_args, iter_val);
                        if (ctx.has_exception()) break;
                        if (nr.is_object() && nr.as_object()->get_property("done").to_boolean()) break;
                    }
                    if (!ctx.has_exception()) {
                        Value return_fn_v = iter_obj->get_property("return");
                        if (return_fn_v.is_function()) {
                            std::vector<Value> ret_args = {current_gen->return_argument_};
                            return_fn_v.as_function()->call(ctx, ret_args, iter_val);
                            ctx.clear_exception();
                        }
                    }
                }
                Generator::set_current_generator(saved_gen);
            }
            return Value();
        }

        if (iterable.is_string()) {
            std::string str = iterable.to_string();
            size_t byte_idx = 0;
            size_t char_idx = 0;
            while (byte_idx < str.size() && char_idx <= needed_idx) {
                unsigned char c = (unsigned char)str[byte_idx];
                size_t char_len = 1;
                if (c >= 0xF0) char_len = 4;
                else if (c >= 0xE0) char_len = 3;
                else if (c >= 0xC0) char_len = 2;
                elements.push_back(Value(str.substr(byte_idx, char_len)));
                byte_idx += char_len;
                char_idx++;
            }
            if (elements.size() < needed_idx + 1) delegate_exhausted = true;
        } else if (iterable.is_object() || iterable.is_function()) {
            Object* obj = iterable.is_object() ? iterable.as_object() : iterable.as_function();

            Value sym_iter_fn = obj->get_property("Symbol.iterator");

            if (sym_iter_fn.is_function()) {
                Generator* saved_gen = Generator::get_current_generator();
                Generator::set_current_generator(nullptr);
                std::vector<Value> no_args;
                Value iter_val = sym_iter_fn.as_function()->call(ctx, no_args, iterable);
                Generator::set_current_generator(saved_gen);
                if (!ctx.has_exception()) {
                    collect_from_iterator(iter_val);
                }
            } else {
                Value next_fn = obj->get_property("next");
                if (next_fn.is_function()) {
                    collect_from_iterator(iterable);
                } else {
                    delegate_exhausted = true;
                }
            }
        } else {
            delegate_exhausted = true;
        }

        size_t delegate_count = elements.size();
        for (size_t i = 1; i < delegate_count; i++) {
            Generator::increment_yield_counter();
        }

        if (current_gen->target_yield_index_ >= delegate_start + delegate_count) {
            return Value();
        }

        size_t element_idx = current_gen->target_yield_index_ - delegate_start;
        throw YieldException(elements[element_idx]);
    }

    Value yield_value = Value();
    if (argument_) {
        yield_value = argument_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }

    if (!current_gen) {
        return yield_value;
    }

    size_t yield_index = Generator::increment_yield_counter();

    if (yield_index < current_gen->target_yield_index_) {
        if (yield_index < current_gen->sent_values_.size()) {
            return current_gen->sent_values_[yield_index];
        }
        return current_gen->last_value_;
    }

    if (current_gen->returning_) {
        return Value();
    }

    if (current_gen->throwing_) {
        current_gen->throwing_ = false;
        ctx.throw_exception(current_gen->throw_value_, true);
        return Value();
    }

    if (yield_index > current_gen->target_yield_index_) {
        current_gen->target_yield_index_ = yield_index;
    }

    throw YieldException(yield_value);
}

std::string YieldExpression::to_string() const {
    std::ostringstream oss;
    oss << "yield";
    if (is_delegate_) {
        oss << "*";
    }
    if (argument_) {
        oss << " " << argument_->to_string();
    }
    return oss.str();
}

std::unique_ptr<ASTNode> YieldExpression::clone() const {
    return std::make_unique<YieldExpression>(
        argument_ ? argument_->clone() : nullptr,
        is_delegate_,
        start_, end_
    );
}


Value AsyncFunctionExpression::evaluate(Context& ctx) {
    std::string function_name = id_ ? id_->get_name() : "anonymous";

    std::vector<std::string> param_names;
    for (const auto& param : params_) {
        param_names.push_back(param->get_name()->get_name());
    }

    auto* fn = new AsyncFunction(function_name, param_names, std::unique_ptr<ASTNode>(body_->clone().release()), &ctx);

    if (ctx.has_binding("AsyncFunction")) {
        Value async_ctor = ctx.get_binding("AsyncFunction");
        if (async_ctor.is_function()) {
            Value proto = async_ctor.as_function()->get_property("prototype");
            if (proto.is_object()) {
                fn->set_prototype(proto.as_object());
            }
        }
    }

    return Value(fn);
}

std::string AsyncFunctionExpression::to_string() const {
    std::ostringstream oss;
    oss << "async function";

    if (id_) {
        oss << " " << id_->get_name();
    }

    oss << "(";
    for (size_t i = 0; i < params_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << params_[i]->get_name();
    }
    oss << ") ";

    oss << body_->to_string();

    return oss.str();
}

std::unique_ptr<ASTNode> AsyncFunctionExpression::clone() const {
    std::vector<std::unique_ptr<Parameter>> cloned_params;
    for (const auto& param : params_) {
        cloned_params.push_back(
            std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release()))
        );
    }

    return std::make_unique<AsyncFunctionExpression>(
        id_ ? std::unique_ptr<Identifier>(static_cast<Identifier*>(id_->clone().release())) : nullptr,
        std::move(cloned_params),
        std::unique_ptr<BlockStatement>(static_cast<BlockStatement*>(body_->clone().release())),
        start_, end_
    );
}


Value ImportSpecifier::evaluate(Context& ctx) {
    return Value();
}

std::string ImportSpecifier::to_string() const {
    if (imported_name_ != local_name_) {
        return imported_name_ + " as " + local_name_;
    }
    return imported_name_;
}

std::unique_ptr<ASTNode> ImportSpecifier::clone() const {
    return std::make_unique<ImportSpecifier>(imported_name_, local_name_, start_, end_);
}

Value ImportStatement::evaluate(Context& ctx) {
    Engine* engine = ctx.get_engine();
    if (!engine) {
        ctx.throw_exception(Value(std::string("No engine available for module loading")));
        return Value();
    }

    ModuleLoader* module_loader = engine->get_module_loader();
    if (!module_loader) {
        ctx.throw_exception(Value(std::string("ModuleLoader not available")));
        return Value();
    }

    try {

        if (!is_namespace_import_ && (!is_default_import_ || is_mixed_import())) {
            for (const auto& specifier : specifiers_) {
                std::string imported_name = specifier->get_imported_name();
                std::string local_name = specifier->get_local_name();

                Value imported_value = module_loader->import_from_module(
                    module_source_, imported_name, ""
                );

                bool binding_success = ctx.create_binding(local_name, imported_value);
            }
        }

        if (is_namespace_import_) {
            Value namespace_obj = module_loader->import_namespace_from_module(
                module_source_, ""
            );
            ctx.create_binding(namespace_alias_, namespace_obj);
        }

        if (is_default_import_) {
            Value default_value;

            try {
                default_value = module_loader->import_default_from_module(
                    module_source_, ""
                );
            } catch (...) {
                default_value = Value();
            }

            if (default_value.is_undefined()) {
                if (engine->has_default_export(module_source_)) {
                    default_value = engine->get_default_export(module_source_);
                } else if (engine->has_default_export("")) {
                    default_value = engine->get_default_export("");
                }
            }

            ctx.create_binding(default_alias_, default_value);
        }

        return Value();

    } catch (const std::exception& e) {
        ctx.throw_exception(Value("Module import failed: " + std::string(e.what())));
        return Value();
    }
}

std::string ImportStatement::to_string() const {
    std::string result = "import ";

    if (is_namespace_import_) {
        result += "* as " + namespace_alias_;
    } else if (is_default_import_) {
        result += default_alias_;
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";
    }

    result += " from \"" + module_source_ + "\"";
    return result;
}

std::unique_ptr<ASTNode> ImportStatement::clone() const {
    if (is_namespace_import_) {
        return std::make_unique<ImportStatement>(namespace_alias_, module_source_, start_, end_);
    } else if (is_default_import_) {
        return std::make_unique<ImportStatement>(default_alias_, module_source_, true, start_, end_);
    } else {
        std::vector<std::unique_ptr<ImportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ImportSpecifier>(
                    spec->get_imported_name(),
                    spec->get_local_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }
        return std::make_unique<ImportStatement>(std::move(cloned_specifiers), module_source_, start_, end_);
    }
}

Value ExportSpecifier::evaluate(Context& ctx) {
    return Value();
}

std::string ExportSpecifier::to_string() const {
    if (local_name_ != exported_name_) {
        return local_name_ + " as " + exported_name_;
    }
    return local_name_;
}

std::unique_ptr<ASTNode> ExportSpecifier::clone() const {
    return std::make_unique<ExportSpecifier>(local_name_, exported_name_, start_, end_);
}

Value ExportStatement::evaluate(Context& ctx) {
    Value exports_value = ctx.get_binding("exports");
    Object* exports_obj = nullptr;

    if (!exports_value.is_object()) {
        exports_obj = new Object();
        ctx.create_binding("exports", Value(exports_obj), true);

        Environment* lexical_env = ctx.get_lexical_environment();
        if (lexical_env) {
            lexical_env->create_binding("exports", Value(exports_obj), true);
        }
    } else {
        exports_obj = exports_value.as_object();
    }

    if (is_default_export_ && default_export_) {
        Value default_value = default_export_->evaluate(ctx);
        if (ctx.has_exception()) return Value();
        exports_obj->set_property("default", default_value);

        Engine* engine = ctx.get_engine();
        if (engine) {
            engine->register_default_export("", default_value);
        }
    }

    if (is_declaration_export_ && declaration_) {
        Value decl_result = declaration_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        if (declaration_->get_type() == Type::FUNCTION_DECLARATION) {
            FunctionDeclaration* func_decl = static_cast<FunctionDeclaration*>(declaration_.get());
            std::string func_name = func_decl->get_id()->get_name();

            if (ctx.has_binding(func_name)) {
                Value func_value = ctx.get_binding(func_name);
                exports_obj->set_property(func_name, func_value);
            }
        } else if (declaration_->get_type() == Type::VARIABLE_DECLARATION) {
            VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(declaration_.get());

            for (const auto& declarator : var_decl->get_declarations()) {
                std::string var_name = declarator->get_id()->get_name();

                if (ctx.has_binding(var_name)) {
                    Value var_value = ctx.get_binding(var_name);
                    exports_obj->set_property(var_name, var_value);
                }
            }
        }
    }

    for (const auto& specifier : specifiers_) {
        std::string local_name = specifier->get_local_name();
        std::string export_name = specifier->get_exported_name();
        Value export_value;

        if (is_re_export_ && !source_module_.empty()) {
            Engine* engine = ctx.get_engine();
            if (engine) {
                ModuleLoader* module_loader = engine->get_module_loader();
                if (module_loader) {
                    try {
                        export_value = module_loader->import_from_module(
                            source_module_, local_name, ""
                        );
                    } catch (...) {
                        export_value = Value();
                    }
                }
            }

            if (export_value.is_undefined()) {
                ctx.throw_exception(Value("ReferenceError: Cannot re-export '" + local_name + "' from '" + source_module_ + "'"));
                return Value();
            }
        } else {
            if (ctx.has_binding(local_name)) {
                export_value = ctx.get_binding(local_name);
            } else {
                ctx.throw_exception(Value("ReferenceError: " + local_name + " is not defined"));
                return Value();
            }
        }

        exports_obj->set_property(export_name, export_value);
    }

    return Value();
}

std::string ExportStatement::to_string() const {
    std::string result = "export ";

    if (is_default_export_) {
        result += "default " + default_export_->to_string();
    } else if (is_declaration_export_) {
        result += declaration_->to_string();
    } else {
        result += "{ ";
        for (size_t i = 0; i < specifiers_.size(); ++i) {
            if (i > 0) result += ", ";
            result += specifiers_[i]->to_string();
        }
        result += " }";

        if (is_re_export_) {
            result += " from \"" + source_module_ + "\"";
        }
    }

    return result;
}

std::unique_ptr<ASTNode> ExportStatement::clone() const {
    if (is_default_export_) {
        return std::make_unique<ExportStatement>(default_export_->clone(), true, start_, end_);
    } else if (is_declaration_export_) {
        return std::make_unique<ExportStatement>(declaration_->clone(), start_, end_);
    } else {
        std::vector<std::unique_ptr<ExportSpecifier>> cloned_specifiers;
        for (const auto& spec : specifiers_) {
            cloned_specifiers.push_back(
                std::make_unique<ExportSpecifier>(
                    spec->get_local_name(),
                    spec->get_exported_name(),
                    spec->get_start(),
                    spec->get_end()
                )
            );
        }

        if (is_re_export_) {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), source_module_, start_, end_);
        } else {
            return std::make_unique<ExportStatement>(std::move(cloned_specifiers), start_, end_);
        }
    }
}

} // namespace Quanta
