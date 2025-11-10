/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/context_core.h"
#include "../../include/Context.h"
#include "../../include/Engine.h"
#include <iostream>

namespace Quanta {

// Static member initialization
uint32_t Context::next_context_id_ = 1;

// Constructor implementation
void ContextCore::construct_context(Context* ctx, Engine* engine, Context::Type type) {
    ctx->type_ = type;
    ctx->state_ = Context::State::Running;
    ctx->context_id_ = Context::next_context_id_++;
    ctx->lexical_environment_ = nullptr;
    ctx->variable_environment_ = nullptr;
    ctx->this_binding_ = nullptr;
    ctx->execution_depth_ = 0;
    ctx->global_object_ = nullptr;
    ctx->has_exception_ = false;
    ctx->has_return_value_ = false;
    ctx->has_break_ = false;
    ctx->has_continue_ = false;
    ctx->strict_mode_ = false;
    ctx->engine_ = engine;
    ctx->current_filename_ = "<unknown>";
    ctx->web_api_interface_ = nullptr;

    if (type == Context::Type::Global) {
        // initialize_global_context() would be called here
    }
}

// Constructor with parent
void ContextCore::construct_context_with_parent(Context* ctx, Engine* engine, Context* parent, Context::Type type) {
    construct_context(ctx, engine, type);

    if (parent) {
        ctx->global_object_ = parent->global_object_;
        ctx->strict_mode_ = parent->strict_mode_;
        ctx->current_filename_ = parent->current_filename_;
        ctx->web_api_interface_ = parent->web_api_interface_;
        ctx->built_in_objects_ = parent->built_in_objects_;
        ctx->built_in_functions_ = parent->built_in_functions_;
    }
}

// Basic context operations
void ContextCore::set_global_object(Context& ctx, Object* global) {
    ctx.global_object_ = global;
}

bool ContextCore::check_execution_depth(const Context& ctx) {
    const int MAX_EXECUTION_DEPTH = 10000;
    return ctx.execution_depth_ < MAX_EXECUTION_DEPTH;
}

void ContextCore::increment_execution_depth(Context& ctx) {
    ctx.execution_depth_++;
}

void ContextCore::decrement_execution_depth(Context& ctx) {
    if (ctx.execution_depth_ > 0) {
        ctx.execution_depth_--;
    }
}

void ContextCore::throw_exception(Context& ctx, const Value& exception) {
    ctx.current_exception_ = exception;
    ctx.has_exception_ = true;
    ctx.state_ = Context::State::Thrown;
}

void ContextCore::clear_exception(Context& ctx) {
    ctx.current_exception_ = Value();
    ctx.has_exception_ = false;
    if (ctx.state_ == Context::State::Thrown) {
        ctx.state_ = Context::State::Running;
    }
}

bool ContextCore::has_pending_exception(const Context& ctx) {
    return ctx.has_exception_;
}

Value ContextCore::get_pending_exception(const Context& ctx) {
    return ctx.current_exception_;
}

// Context state management
void ContextCore::initialize_core_context(Context& ctx) {
    clear_exception(ctx);
}

void ContextCore::reset_context(Context& ctx) {
    clear_exception(ctx);
    ctx.has_return_value_ = false;
    ctx.has_break_ = false;
    ctx.has_continue_ = false;
    ctx.execution_depth_ = 0;
}

bool ContextCore::is_context_valid(const Context& ctx) {
    return ctx.state_ != Context::State::Completed && ctx.engine_ != nullptr;
}

// Stack management (placeholder implementations)
void ContextCore::push_frame(Context& ctx, std::unique_ptr<StackFrame> frame) {
    // Would push to ctx.call_stack_
}

std::unique_ptr<StackFrame> ContextCore::pop_frame(Context& ctx) {
    // Would pop from ctx.call_stack_
    return nullptr;
}

StackFrame* ContextCore::get_current_frame(Context& ctx) {
    // Would return top of ctx.call_stack_
    return nullptr;
}

size_t ContextCore::get_stack_depth(const Context& ctx) {
    return ctx.call_stack_.size();
}

void ContextCore::print_stack_trace(const Context& ctx) {
    std::cerr << "Stack trace: " << ctx.call_stack_.size() << " frames" << std::endl;
    // Would iterate through ctx.call_stack_ and print each frame
}

} // namespace Quanta