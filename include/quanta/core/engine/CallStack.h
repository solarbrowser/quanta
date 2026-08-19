/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_CALL_STACK_H
#define QUANTA_CALL_STACK_H

#include <vector>
#include <span>
#include <string>
#include <memory>
#include "quanta/lexer/Token.h"

namespace Quanta {

class Function;
class Object;
class ASTNode;

class CallStack;

struct CallStackFrame {
    // A frame is built on every call and read only when a stack trace is
    // formatted, so it stores nothing it can derive. It used to copy the
    // callee's name eagerly, and reaching for that name touched a cache line
    // of the Function's instance data that the hot path never touches
    // otherwise -- one dependent load, cold on every call once a program has
    // more functions than fit in cache.
    const std::string* filename = nullptr;
    Function* function_ptr = nullptr;

    CallStackFrame() = default;
    CallStackFrame(const std::string* file, Function* func = nullptr)
        : filename(file), function_ptr(func) {}

    // Both derived from function_ptr, which outlives the frame: frames are
    // strictly LIFO within a live call.
    const std::string& name() const;
    Position position() const;

    std::string to_string() const;
};

/**
 * Manages the JavaScript call stack for error reporting and debugging
 */
class CallStack {
private:
    // Sized once to the depth limit rather than grown: a frame is pushed and
    // popped on every call, and letting the vector own that meant a capacity
    // test and a size update on the way in and an emptiness test on the way
    // out, for a container whose bound is a compile-time constant.
    std::vector<CallStackFrame> frames_;
    size_t depth_ = 0;
    static constinit thread_local CallStack* instance_;
    static void init_default_instance();
    
public:
    // The point at which a JS call is refused with a RangeError. Has to stay
    // under what the C++ stack beneath the interpreter can actually take.
    static constexpr size_t MAX_STACK_DEPTH = 3000;

    CallStack() : frames_(MAX_STACK_DEPTH) {}
    ~CallStack() = default;
    
    // Inline for the same reason push_frame is: this is read once per JS call,
    // and the lazy default lives behind a cold out-of-line helper so the hot
    // path is a thread-local load and a null test rather than a call whose
    // body carries a function-scope thread_local's guard.
    static CallStack& instance() {
        if (!instance_) init_default_instance();
        return *instance_;
    }
    static void set_instance(CallStack* stack);
    
    // Inline, and deliberately so: a frame is two pointers, but reaching a
    // push in another translation unit cost more than the push itself -- an
    // out-of-line call each way on every JS call, for a vector append the
    // caller could have done in a handful of instructions.
    void push_frame(const std::string* filename, Function* function_ptr = nullptr) {
        if (depth_ >= MAX_STACK_DEPTH) return;
        frames_[depth_].filename = filename;
        frames_[depth_].function_ptr = function_ptr;
        ++depth_;
    }

    // For a caller that has already refused the call past MAX_STACK_DEPTH:
    // the bound is established before this runs, and the matching pop knows a
    // frame is there because this put one there.
    void push_frame_unchecked(const std::string* filename, Function* function_ptr) {
        frames_[depth_].filename = filename;
        frames_[depth_].function_ptr = function_ptr;
        ++depth_;
    }

    void pop_frame() {
        if (depth_) --depth_;
    }

    void pop_frame_unchecked() { --depth_; }

    void clear();
    
    size_t depth() const { return depth_; }
    bool is_empty() const { return depth_ == 0; }
    bool is_full() const { return depth_ >= MAX_STACK_DEPTH; }
    
    const CallStackFrame& top() const;
    const CallStackFrame& at(size_t index) const;
    std::span<const CallStackFrame> frames() const { return {frames_.data(), depth_}; }
    
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
// Pairs with push_frame_unchecked: for a call site that tested the depth
// itself, which is every site that has to report the overflow as a RangeError
// rather than let the push quietly drop the frame.
class CheckedDepthFrameGuard {
public:
    CheckedDepthFrameGuard(CallStack& stack, const std::string* filename, Function* function_ptr)
        : stack_(stack) { stack_.push_frame_unchecked(filename, function_ptr); }
    ~CheckedDepthFrameGuard() { stack_.pop_frame_unchecked(); }
    CheckedDepthFrameGuard(const CheckedDepthFrameGuard&) = delete;
    CheckedDepthFrameGuard& operator=(const CheckedDepthFrameGuard&) = delete;
private:
    CallStack& stack_;
};

class CallStackFrameGuard {
private:
    CallStack& stack_;
    
public:
    CallStackFrameGuard(CallStack& stack, const std::string* filename,
                   Function* function_ptr = nullptr)
        : stack_(stack) {
        stack_.push_frame(filename, function_ptr);
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
