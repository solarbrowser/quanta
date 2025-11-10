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
#include "../../lexer/include/Token.h"

namespace Quanta {

class Function;
class ASTNode;

/**
 * Represents a single frame in the call stack
 */
struct CallStackFrame {
    std::string function_name;      // Name of the function being called
    std::string filename;           // Source file name
    Position position;              // Line/column in source
    Function* function_ptr;         // Pointer to the function object (can be null)
    ASTNode* call_site;             // AST node where the call was made (can be null)
    
    CallStackFrame(const std::string& name, const std::string& file, const Position& pos,
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
    
    // Maximum stack depth to prevent infinite recursion
    static constexpr size_t MAX_STACK_DEPTH = 1000;
    
public:
    CallStack() = default;
    ~CallStack() = default;
    
    // Singleton access
    static CallStack& instance();
    static void set_instance(CallStack* stack);
    
    // Stack management
    void push_frame(const std::string& function_name, 
                   const std::string& filename, 
                   const Position& position,
                   Function* function_ptr = nullptr,
                   ASTNode* call_site = nullptr);
    
    void pop_frame();
    void clear();
    
    // Stack inspection  
    size_t depth() const { return frames_.size(); }
    bool is_empty() const { return frames_.empty(); }
    bool is_full() const { return frames_.size() >= MAX_STACK_DEPTH; }
    
    const CallStackFrame& top() const;
    const CallStackFrame& at(size_t index) const;
    const std::vector<CallStackFrame>& frames() const { return frames_; }
    
    // Stack trace generation
    std::string generate_stack_trace() const;
    std::string generate_stack_trace(size_t max_frames) const;
    
    // Current location helpers
    std::string current_function() const;
    std::string current_filename() const;
    Position current_position() const;
    
    // Stack overflow detection
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
                   const std::string& filename, const Position& position,
                   Function* function_ptr = nullptr, ASTNode* call_site = nullptr)
        : stack_(stack) {
        stack_.push_frame(function_name, filename, position, function_ptr, call_site);
    }
    
    ~CallStackFrameGuard() {
        stack_.pop_frame();
    }
    
    // Non-copyable, non-movable
    CallStackFrameGuard(const CallStackFrameGuard&) = delete;
    CallStackFrameGuard& operator=(const CallStackFrameGuard&) = delete;
    CallStackFrameGuard(CallStackFrameGuard&&) = delete;
    CallStackFrameGuard& operator=(CallStackFrameGuard&&) = delete;
};

// Convenience macro for stack frame management
#define STACK_FRAME(name, file, pos) \
    CallStackFrameGuard __frame_guard(CallStack::instance(), name, file, pos)

#define STACK_FRAME_FUNC(name, file, pos, func) \
    CallStackFrameGuard __frame_guard(CallStack::instance(), name, file, pos, func)

} // namespace Quanta

#endif // QUANTA_CALL_STACK_H