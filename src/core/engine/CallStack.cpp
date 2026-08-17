/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/CallStack.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"
#include <sstream>
#include <algorithm>

namespace Quanta {

constinit thread_local CallStack* CallStack::instance_ = nullptr;

std::string resolve_private_storage_key(const std::string& bare_name, Object* obj) {
    CallStack& cs = CallStack::instance();
    for (size_t i = cs.depth(); i > 0; --i) {
        Function* fn = cs.at(i - 1).function_ptr;
        if (!fn) continue;
        Object* brands_obj = fn->private_brands();
        if (!brands_obj) continue;
        Value name_brand = brands_obj->get_property(bare_name);
        if (!name_brand.is_object() && !name_brand.is_function()) continue;
        Object* expected = name_brand.is_function()
            ? static_cast<Object*>(name_brand.as_function())
            : name_brand.as_object();
        return bare_name + "@" + std::to_string(reinterpret_cast<uintptr_t>(expected));
    }
    // No frame declares this name -- typical after resuming an async function or
    // generator past an await/yield, where the continuation doesn't re-enter
    // through Function::call. Fall back to whatever qualified slot already exists
    // on the object or its prototype chain (methods/accessors live on the
    // declaring prototype/constructor). Raw scan: get_own_property_keys hides
    // qualified slots from observable enumeration.
    if (obj) {
        std::string prefix = bare_name + "@";
        for (Object* o = obj; o; o = o->get_prototype()) {
            std::string k = o->find_private_slot_key(prefix);
            if (!k.empty()) return k;
        }
    }
    return bare_name;
}

Object* resolve_private_accessor_owner(const std::string& bare_name) {
    CallStack& cs = CallStack::instance();
    for (size_t i = cs.depth(); i > 0; --i) {
        Function* fn = cs.at(i - 1).function_ptr;
        if (!fn) continue;
        Object* brands_obj = fn->private_brands();
        if (!brands_obj) continue;
        Value name_brand = brands_obj->get_property(bare_name);
        if (!name_brand.is_object() && !name_brand.is_function()) continue;
        return name_brand.is_function() ? static_cast<Object*>(name_brand.as_function()) : name_brand.as_object();
    }
    return nullptr;
}

const std::string& CallStackFrame::name() const {
    static const std::string kEmpty;
    return function_ptr ? function_ptr->get_name() : kEmpty;
}

Position CallStackFrame::position() const {
    const ASTNode* body = function_ptr ? function_ptr->ast_body() : nullptr;
    return body ? body->get_start() : Position(1, 1, 0);
}

std::string CallStackFrame::to_string() const {
    std::ostringstream oss;
    const Position pos = position();
    oss << "at ";
    
    if (!name().empty()) {
        oss << name();
    } else {
        oss << "<anonymous>";
    }
    
    if (filename && !filename->empty()) {
        oss << " (" << *filename;
        if (pos.line > 0) {
            oss << ":" << pos.line;
            if (pos.column > 0) {
                oss << ":" << pos.column;
            }
        }
        oss << ")";
    }

    return oss.str();
}

void CallStack::init_default_instance() {
    static thread_local CallStack default_instance;
    instance_ = &default_instance;
}

void CallStack::set_instance(CallStack* stack) {
    instance_ = stack;
}

void CallStack::clear() {
    frames_.clear();
}

const CallStackFrame& CallStack::top() const {
    if (frames_.empty()) {
        static CallStackFrame empty_frame(nullptr, nullptr);
        return empty_frame;
    }
    return frames_.back();
}

const CallStackFrame& CallStack::at(size_t index) const {
    if (index >= frames_.size()) {
        static CallStackFrame empty_frame(nullptr, nullptr);
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
    
    for (size_t i = 0; i < frame_count; ++i) {
        size_t frame_idx = frames_.size() - 1 - i;
        oss << "    " << format_frame(frames_[frame_idx], i);
        if (i < frame_count - 1) {
            oss << "\n";
        }
    }
    
    if (max_frames < frames_.size()) {
        oss << "\n    ... and " << (frames_.size() - max_frames) << " more frames";
    }
    
    return oss.str();
}

std::string CallStack::current_function() const {
    if (frames_.empty()) {
        return "<global>";
    }
    const std::string& n = frames_.back().name();
    return n.empty() ? "<anonymous>" : n;
}

std::string CallStack::current_filename() const {
    if (frames_.empty()) {
        return "<unknown>";
    }
    const std::string* f = frames_.back().filename;
    return (f && !f->empty()) ? *f : "<unknown>";
}

Position CallStack::current_position() const {
    if (frames_.empty()) {
        return Position();
    }
    return frames_.back().position();
}

bool CallStack::check_stack_overflow() {
    if (is_full()) {
        return true;
    }
    return false;
}

std::string CallStack::format_frame(const CallStackFrame& frame, size_t index) const {
    std::ostringstream oss;
    oss << "at ";
    
    const Position frame_pos = frame.position();
    if (!frame.name().empty()) {
        oss << frame.name();
    } else {
        oss << "<anonymous>";
    }
    
    if (frame.filename && !frame.filename->empty()) {
        oss << " (" << *frame.filename;
        if (frame_pos.line > 0) {
            oss << ":" << frame_pos.line;
            if (frame_pos.column > 0) {
                oss << ":" << frame_pos.column;
            }
        }
        oss << ")";
    } else {
        oss << " (<unknown>)";
    }
    
    return oss.str();
}

}
