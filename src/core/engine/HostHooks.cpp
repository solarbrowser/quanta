/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/HostHooks.h"
#include "quanta/core/engine/CallStack.h"
#include <iostream>

namespace Quanta {

namespace {

HostHooks::UncaughtFn& uncaught_handler() {
    static thread_local HostHooks::UncaughtFn fn;
    return fn;
}

HostHooks::RejectionFn& rejection_tracker() {
    static thread_local HostHooks::RejectionFn fn;
    return fn;
}

constinit thread_local size_t g_script_depth = 0;

}

void HostHooks::set_uncaught_handler(UncaughtFn fn) { uncaught_handler() = std::move(fn); }
void HostHooks::set_rejection_tracker(RejectionFn fn) { rejection_tracker() = std::move(fn); }

void HostHooks::report_uncaught(Context& ctx, const Value& exception, const char* origin) {
    if (UncaughtFn& handler = uncaught_handler()) {
        // Copied: the handler may install another handler, or run script that does.
        UncaughtFn call = handler;
        call(ctx, exception, origin);
        return;
    }
    std::cerr << "Uncaught (in " << origin << ") " << exception.to_string() << std::endl;
}

bool HostHooks::has_rejection_tracker() { return static_cast<bool>(rejection_tracker()); }

void HostHooks::note_rejection(Promise* promise, bool handled) {
    if (RejectionFn& tracker = rejection_tracker()) {
        RejectionFn call = tracker;
        call(promise, handled);
    }
}

HostHooks::ScriptEntry::ScriptEntry() { g_script_depth++; }
HostHooks::ScriptEntry::~ScriptEntry() { g_script_depth--; }

bool HostHooks::js_stack_empty() {
    return g_script_depth == 0 && CallStack::instance().depth() == 0;
}

}
