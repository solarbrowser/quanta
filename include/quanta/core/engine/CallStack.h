/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_CALL_STACK_H
#define QUANTA_CALL_STACK_H

#include <vector>
#include <string>
#include <memory>
#include "quanta/lexer/Token.h"

namespace Quanta {

class Function;
class Object;
class ASTNode;

struct CallStackFrame {
    std::string function_name;
    const std::string* filename;
    Position position;
    Function* function_ptr;
    ASTNode* call_site;

    CallStackFrame(const std::string& name, const std::string* file, const Position& pos,
               Function* func = nullptr, ASTNode* call = nullptr)
        : function_name(name), filename(file), position(pos),
          function_ptr(func), call_site(call) {}

    std::string to_string() const;
};

/**
 * Manages the JavaScript call stack for error reporting and debugging
 */
class CallStack {
private:
    std::vector<CallStackFrame> frames_;
    static thread_local CallStack* instance_;
    
    static constexpr size_t MAX_STACK_DEPTH = 1000;
    
public:
    CallStack() = default;
    ~CallStack() = default;
    
    static CallStack& instance();
    static void set_instance(CallStack* stack);
    
    void push_frame(const std::string& function_name,
                   const std::string* filename,
                   const Position& position,
                   Function* function_ptr = nullptr,
                   ASTNode* call_site = nullptr);
    
    void pop_frame();
    void clear();
    
    size_t depth() const { return frames_.size(); }
    bool is_empty() const { return frames_.empty(); }
    bool is_full() const { return frames_.size() >= MAX_STACK_DEPTH; }
    
    const CallStackFrame& top() const;
    const CallStackFrame& at(size_t index) const;
    const std::vector<CallStackFrame>& frames() const { return frames_; }
    
    std::string generate_stack_trace() const;
    std::string generate_stack_trace(size_t max_frames) const;
    
    std::string current_function() const;
    std::string current_filename() const;
    Position current_position() const;
    
    bool check_stack_overflow();
    
private:
    std::string format_frame(const CallStackFrame& frame, size_t index = 0) const;
};

/**
 * RAII helper for managing stack frames
 */
class CallStackFrameGuard {
private:
    CallStack& stack_;
    
public:
    CallStackFrameGuard(CallStack& stack, const std::string& function_name,
                   const std::string* filename, const Position& position,
                   Function* function_ptr = nullptr, ASTNode* call_site = nullptr)
        : stack_(stack) {
        stack_.push_frame(function_name, filename, position, function_ptr, call_site);
    }
    
    ~CallStackFrameGuard() {
        stack_.pop_frame();
    }
    
    CallStackFrameGuard(const CallStackFrameGuard&) = delete;
    CallStackFrameGuard& operator=(const CallStackFrameGuard&) = delete;
    CallStackFrameGuard(CallStackFrameGuard&&) = delete;
    CallStackFrameGuard& operator=(CallStackFrameGuard&&) = delete;
};

#define STACK_FRAME(name, file, pos) \
    CallStackFrameGuard __frame_guard(CallStack::instance(), name, file, pos)

#define STACK_FRAME_FUNC(name, file, pos, func) \
    CallStackFrameGuard __frame_guard(CallStack::instance(), name, file, pos, func)

// Qualifies a bare private name (e.g. "#x") with its declaring class's brand (e.g. "#x@1234") so a base and derived class's same-named private fields don't collide into one storage slot. Falls back to scanning obj's own slots, then to the bare name, if the call stack has no frame declaring it (e.g. resumed after an await/yield).
std::string resolve_private_storage_key(const std::string& bare_name, Object* obj = nullptr);

// Like resolve_private_storage_key, but returns the declaring class's prototype directly --
// for private accessors/methods, which live on that exact prototype, not on obj's own slots.
Object* resolve_private_accessor_owner(const std::string& bare_name);

// PrivateBrandCheck: does an active frame's brand admit `prop_name` on obj?
// Defined in member.cpp; shared by the tree-walker's private member paths
// and the VM's Op::GetPrivate/SetPrivate slow path.
class Context;
bool private_brand_check(Context& ctx, Object* obj, const std::string& prop_name, bool require_exists = true);

}

#endif
