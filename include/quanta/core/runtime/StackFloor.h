#pragma once

#include <cstddef>

namespace Quanta {

// The lowest stack address code running right now may use. Zero means "this is
// the thread's own stack", whose extent nobody here tracks -- the parser falls
// back to measuring how far it has come from where it started. A fiber is the
// case that needs telling: its stack is a fixed allocation, far smaller than
// the thread's, and a budget derived from the thread's limit would let a deep
// recursion run straight off the end of it.
//
// Set and restored around a fiber switch, so it describes whichever stack is
// executing rather than whichever one started the work.
inline constinit thread_local const char* g_stack_floor = nullptr;

inline const char* current_stack_floor() { return g_stack_floor; }

// Sets the floor for as long as the fiber runs, and puts back whatever the
// caller was on when it yields or finishes.
class StackFloorScope {
public:
    StackFloorScope(const char* base, size_t size)
        : saved_(g_stack_floor) {
        // Room for the frame that notices it has to stop, for the error it
        // reports, and for taking apart the tree built so far -- that unwinds
        // recursively too, though at a fraction of a frame per level compared
        // with building it.
        constexpr size_t kMargin = 32u * 1024;
        g_stack_floor = (base && size > kMargin) ? base + kMargin : nullptr;
    }
    ~StackFloorScope() { g_stack_floor = saved_; }
    StackFloorScope(const StackFloorScope&) = delete;
    StackFloorScope& operator=(const StackFloorScope&) = delete;

private:
    const char* saved_;
};

}  // namespace Quanta
