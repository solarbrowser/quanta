/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "CallStack.h"
#include "../../parser/include/AST.h"
#include <sstream>
#include <algorithm>

namespace Quanta {

// Thread-local instance
thread_local CallStack* CallStack::instance_ = nullptr;

std::string CallStackFrame::to_string() const {
    std::ostringstream oss;
    oss << "at ";
    
    if (!function_name.empty()) {
        oss << function_name;
    } else {
        oss << "<anonymous>";
    }
    
    if (!filename.empty()) {
        oss << " (" << filename;
        if (position.line > 0) {
            oss << ":" << position.line;
            if (position.column > 0) {
                oss << ":" << position.column;
            }
        }
        oss << ")";
    }
    
    return oss.str();
}

CallStack& CallStack::instance() {
    if (!instance_) {
        static CallStack default_instance;
        instance_ = &default_instance;
    }
    return *instance_;
}

void CallStack::set_instance(CallStack* stack) {
    instance_ = stack;
}

void CallStack::push_frame(const std::string& function_name,
                          const std::string& filename,
                          const Position& position,
                          Function* function_ptr,
                          ASTNode* call_site) {
    // Check for stack overflow
    if (is_full()) {
        // Don't add more frames, but don't crash either
        return;
    }
    
    frames_.emplace_back(function_name, filename, position, function_ptr, call_site);
}

void CallStack::pop_frame() {
    if (!frames_.empty()) {
        frames_.pop_back();
    }
}

void CallStack::clear() {
    frames_.clear();
}

const CallStackFrame& CallStack::top() const {
    if (frames_.empty()) {
        static CallStackFrame empty_frame("", "", Position());
        return empty_frame;
    }
    return frames_.back();
}

const CallStackFrame& CallStack::at(size_t index) const {
    if (index >= frames_.size()) {
        static CallStackFrame empty_frame("", "", Position());
        return empty_frame;
    }
    return frames_[index];
}

std::string CallStack::generate_stack_trace() const {
    return generate_stack_trace(frames_.size());
}

std::string CallStack::generate_stack_trace(size_t max_frames) const {
    if (frames_.empty()) {
        return "";
    }
    
    std::ostringstream oss;
    size_t frame_count = std::min(max_frames, frames_.size());
    
    // Display frames in reverse order (most recent first)
    for (size_t i = 0; i < frame_count; ++i) {
        size_t frame_idx = frames_.size() - 1 - i;
        oss << "    " << format_frame(frames_[frame_idx], i);
        if (i < frame_count - 1) {
            oss << "\n";
        }
    }
    
    // If we truncated the stack trace, show how many frames were omitted
    if (max_frames < frames_.size()) {
        oss << "\n    ... and " << (frames_.size() - max_frames) << " more frames";
    }
    
    return oss.str();
}

std::string CallStack::current_function() const {
    if (frames_.empty()) {
        return "<global>";
    }
    return frames_.back().function_name.empty() ? "<anonymous>" : frames_.back().function_name;
}

std::string CallStack::current_filename() const {
    if (frames_.empty()) {
        return "<unknown>";
    }
    return frames_.back().filename.empty() ? "<unknown>" : frames_.back().filename;
}

Position CallStack::current_position() const {
    if (frames_.empty()) {
        return Position();
    }
    return frames_.back().position;
}

bool CallStack::check_stack_overflow() {
    if (is_full()) {
        // Generate a stack overflow error
        // This should be handled by the caller
        return true;
    }
    return false;
}

std::string CallStack::format_frame(const CallStackFrame& frame, size_t index) const {
    std::ostringstream oss;
    oss << "at ";
    
    // Function name
    if (!frame.function_name.empty()) {
        oss << frame.function_name;
    } else {
        oss << "<anonymous>";
    }
    
    // Location information
    if (!frame.filename.empty()) {
        oss << " (" << frame.filename;
        if (frame.position.line > 0) {
            oss << ":" << frame.position.line;
            if (frame.position.column > 0) {
                oss << ":" << frame.position.column;
            }
        }
        oss << ")";
    } else {
        oss << " (<unknown>)";
    }
    
    return oss.str();
}

} // namespace Quanta