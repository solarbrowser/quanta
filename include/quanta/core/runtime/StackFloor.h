#pragma once

#include <cstddef>
#if defined(__linux__)
#include <pthread.h>
#endif

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

// Whether the thread's own stack is nearly spent (Linux; elsewhere never). The frame count that guards recursion (CallStack::MAX_STACK_DEPTH) is a stand-in calibrated for
// frames of a size that a build, a profile-guided inlining decision or a deep eval path can exceed: the thread's stack then ends before the count does. The end is read once per
// thread (what the system gave it, or what `ulimit -s` allows the main thread), and the margin leaves room for the frames that report the error.
inline bool thread_stack_low() {
#if defined(__linux__)
    static thread_local const char* floor = nullptr;
    static thread_local bool known = false;
    if (__builtin_expect(!known, 0)) {
        known = true;
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* addr = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0 && size > (512u << 10)) floor = static_cast<const char*>(addr) + (256u << 10);
            pthread_attr_destroy(&attr);
        }
    }
    if (g_stack_floor) return false;      // (on a fiber's stack, which the floor of its scope guards: the thread's addresses say nothing about it)
    const char probe = 0;
    return floor && &probe < floor;
#else
    return false;
#endif
}

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
