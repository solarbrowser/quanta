/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Context.h"
#include <memory>

namespace Quanta {

class StackFrame;

/**
 * Core context management functionality
 * Handles stack frames, exception management, and basic context operations
 */
class ContextCore {
public:
    /**
     * Context construction
     */
    static void construct_context(Context* ctx, Engine* engine, Context::Type type);
    static void construct_context_with_parent(Context* ctx, Engine* engine, Context* parent, Context::Type type);

    /**
     * Initialize core context functionality
     */
    static void initialize_core_context(Context& ctx);

    /**
     * Basic context operations
     */
    static void set_global_object(Context& ctx, Object* global);
    static bool check_execution_depth(const Context& ctx);
    static void increment_execution_depth(Context& ctx);
    static void decrement_execution_depth(Context& ctx);

    /**
     * Stack frame management
     */
    static void push_frame(Context& ctx, std::unique_ptr<StackFrame> frame);
    static std::unique_ptr<StackFrame> pop_frame(Context& ctx);
    static StackFrame* get_current_frame(Context& ctx);

    /**
     * Exception handling
     */
    static void throw_exception(Context& ctx, const Value& exception);
    static bool has_pending_exception(const Context& ctx);
    static Value get_pending_exception(const Context& ctx);
    static void clear_exception(Context& ctx);

    /**
     * Context state management
     */
    static void reset_context(Context& ctx);
    static bool is_context_valid(const Context& ctx);

    /**
     * Debugging and introspection
     */
    static size_t get_stack_depth(const Context& ctx);
    static void print_stack_trace(const Context& ctx);
};

} // namespace Quanta