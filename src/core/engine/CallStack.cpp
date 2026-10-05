/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/CallStack.h"
#include "quanta/parser/ScriptUnit.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/vm/Bytecode.h"
#include "quanta/core/engine/Context.h"
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
    // Recorded when the body was attached, so asking where a frame is does not
    // require the body still to be there -- materializing it here is what kept
    // every compiled function's tree alive for the whole run.
    return function_ptr ? function_ptr->body_start_position() : Position(1, 1, 0);
}

const std::string* CallStackFrame::file() const {
    if (function_ptr && !function_ptr->is_native()) {
        if (const FunctionExecutable* exe = function_ptr->get_executable().get()) {
            if (const ScriptUnit* unit = exe->script_unit()) {
                if (!unit->filename().empty()) return &unit->filename();
            }
        }
    }
    return filename;
}

std::string CallStackFrame::to_string() const {
    const Position pos = position();
    std::string out = "at ";
    out += name().empty() ? "<anonymous>" : name();

    if (const std::string* file_name = file(); file_name && !file_name->empty()) {
        out += " (";
        out += *file_name;
        if (pos.line > 0) {
            out += ":";
            out += std::to_string(pos.line);
            if (pos.column > 0) {
                out += ":";
                out += std::to_string(pos.column);
            }
        }
        out += ")";
    }

    return out;
}

void CallStack::init_default_instance() {
    static thread_local CallStack default_instance;
    instance_ = &default_instance;
}

void CallStack::set_instance(CallStack* stack) {
    instance_ = stack;
}

void CallStack::clear() {
    depth_ = 0;
}

const CallStackFrame& CallStack::top() const {
    if (depth_ == 0) {
        static CallStackFrame empty_frame(nullptr, nullptr);
        return empty_frame;
    }
    return frames_[depth_ - 1];
}

const CallStackFrame& CallStack::at(size_t index) const {
    if (index >= depth_) {
        static CallStackFrame empty_frame(nullptr, nullptr);
        return empty_frame;
    }
    return frames_[index];
}

std::string CallStack::generate_stack_trace() const {
    return generate_stack_trace(depth_);
}

std::string CallStack::generate_stack_trace(size_t max_frames) const {
    return generate_stack_trace(max_frames, 0);
}

namespace {

struct ResolvedPosition {
    bool known = false;
    uint32_t line = 0, column = 0;
};

// Where each frame of the stack is, from the bytecode frames running now, when tracking is on. A frame
// of script code takes the innermost frame not yet taken that belongs to its function; a native has none.
// What is left once the calls are matched is the script's own top level, if there is one.
const VM::FrameLink* resolve_positions(const CallStack& stack, size_t depth, std::vector<ResolvedPosition>& out) {
    out.assign(depth, ResolvedPosition{});
    const VM::FrameLink* link = VM::g_frame_links;
    if (!link) return nullptr;
    for (size_t idx = depth; idx > 0; --idx) {
        const CallStackFrame& frame = stack.at(idx - 1);
        if (!frame.function_ptr || frame.function_ptr->is_native()) continue;
        const VM::FrameLink* match = link;
        while (match && match->owner != frame.function_ptr) match = match->prev;
        if (!match) continue;
        uint32_t line = 0, column = 0;
        if (match->chunk->position_at(*match->pc, line, column)) out[idx - 1] = {true, line, column};
        link = match->prev;
    }
    return link;
}

}

std::string CallStack::generate_stack_trace(size_t max_frames, size_t skip_top) const {
    if (depth_ <= skip_top && !VM::g_frame_links) {
        return "";
    }

    std::string trace;
    std::vector<ResolvedPosition> resolved;
    const VM::FrameLink* rest = resolve_positions(*this, depth_, resolved);
    const size_t available = depth_ > skip_top ? depth_ - skip_top : 0;
    size_t frame_count = std::min(max_frames, available);

    for (size_t i = 0; i < frame_count; ++i) {
        size_t frame_idx = depth_ - 1 - skip_top - i;
        trace += "    ";
        Position actual(resolved[frame_idx].line, resolved[frame_idx].column, 0);
        trace += format_frame(frames_[frame_idx], i, resolved[frame_idx].known ? &actual : nullptr);
        if (i < frame_count - 1) {
            trace += "\n";
        }
    }

    if (max_frames < available) {
        trace += "\n    ... and ";
        trace += std::to_string(available - max_frames);
        trace += " more frames";
    } else if (rest && !rest->owner && frame_count < max_frames) {
        // The script itself, outside every function.
        uint32_t line = 0, column = 0;
        if (rest->chunk->position_at(*rest->pc, line, column)) {
            Context* ctx = Object::current_context_;
            const std::string file = ctx ? ctx->get_current_filename() : std::string();
            if (frame_count > 0) trace += "\n";
            trace += "    at <anonymous>";
            if (!file.empty()) {
                trace += " (" + file + ":" + std::to_string(line) + ":" + std::to_string(column) + ")";
            }
        }
    }

    return trace;
}

std::string CallStack::current_function() const {
    if (depth_ == 0) {
        return "<global>";
    }
    const std::string& n = frames_[depth_ - 1].name();
    return n.empty() ? "<anonymous>" : n;
}

std::string CallStack::current_filename() const {
    if (depth_ == 0) {
        return "<unknown>";
    }
    const std::string* f = frames_[depth_ - 1].file();
    return (f && !f->empty()) ? *f : "<unknown>";
}

Position CallStack::current_position() const {
    if (depth_ == 0) {
        return Position();
    }
    return frames_[depth_ - 1].position();
}

bool CallStack::check_stack_overflow() {
    if (is_full()) {
        return true;
    }
    return false;
}

std::string CallStack::format_frame(const CallStackFrame& frame, size_t index) const {
    return format_frame(frame, index, nullptr);
}

std::string CallStack::format_frame(const CallStackFrame& frame, size_t index, const Position* actual) const {
    std::string out = "at ";

    const Position frame_pos = actual ? *actual : frame.position();
    out += frame.name().empty() ? "<anonymous>" : frame.name();

    if (const std::string* file_name = frame.file(); file_name && !file_name->empty()) {
        out += " (";
        out += *file_name;
        if (frame_pos.line > 0) {
            out += ":";
            out += std::to_string(frame_pos.line);
            if (frame_pos.column > 0) {
                out += ":";
                out += std::to_string(frame_pos.column);
            }
        }
        out += ")";
    } else {
        out += " (<unknown>)";
    }

    return out;
}

}
