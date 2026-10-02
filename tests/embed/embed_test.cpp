/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Acceptance tests for the embedding surface, written the way an embedder
// would use it: through quanta/Embed.h only, plus the collector's own entry
// points to force the collections a test needs to observe.

#include "quanta/Embed.h"
#include "quanta/core/gc/Collector.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace Quanta;
using namespace Quanta::Embed;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// ---- Lifetimes ---------------------------------------------------------------

static int g_counted_alive = 0;
static int g_counted_destroyed = 0;
static int g_holder_alive = 0;

// The std::string is longer than any small-string buffer, so it owns malloc
// memory that only the destructor returns.
struct Counted : DOMObject {
    std::string label = "a label long enough that it cannot live inside the string object itself";
    Counted() { g_counted_alive++; }
    ~Counted() {
        g_counted_alive--;
        g_counted_destroyed++;
    }
};

struct Holder : DOMObject {
    Counted* child = nullptr;
    Holder() { g_holder_alive++; }
    ~Holder() { g_holder_alive--; }
    void Visit(Visitor& v) { v.Mark(child); }
};

// Overwrites the stack below the caller, where a finished helper's locals still
// sit. The collector scans conservatively, so a stale copy of a pointer the
// test has dropped would keep its cell alive and the test would be measuring
// the scan rather than the guarantee.
__attribute__((noinline)) static void scrub_stack() {
    volatile char buf[32768];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
}

__attribute__((noinline)) static void make_garbage(int n) {
    for (int i = 0; i < n; i++) Heap::Allocate<Counted>();
}

__attribute__((noinline)) static void make_rooted_pair(Context& ctx) {
    Holder* holder = Heap::Allocate<Holder>();
    holder->child = Heap::Allocate<Counted>();
    Set(ctx, Value(ctx.get_global_object()), "holder", FromObject(holder));
}

static void collect(Runtime& rt) {
    scrub_stack();
    rt.CollectGarbage();
}

static void test_destructor_runs_on_sweep(Runtime& rt) {
    const int before = g_counted_destroyed;
    make_garbage(500);
    CHECK(g_counted_alive == 500);
    collect(rt);
    CHECK(g_counted_destroyed - before == 500);
    CHECK(g_counted_alive == 0);
}

static void test_visit_keeps_children_alive(Runtime& rt) {
    Context& ctx = rt.GetContext();
    make_rooted_pair(ctx);
    collect(rt);
    CHECK(g_holder_alive == 1);
    CHECK(g_counted_alive == 1);

    // The child is reachable only through Visit(): drop the holder and both go.
    Set(ctx, Value(ctx.get_global_object()), "holder", Null());
    collect(rt);
    CHECK(g_holder_alive == 0);
    CHECK(g_counted_alive == 0);
}

__attribute__((noinline)) static Holder* make_old_holder(Context& ctx, Runtime& rt) {
    Holder* holder = Heap::Allocate<Holder>();
    Set(ctx, Value(ctx.get_global_object()), "oldHolder", FromObject(holder));
    collect(rt);
    return holder;
}

__attribute__((noinline)) static void give_young_child(Holder* holder) {
    holder->child = Heap::Allocate<Counted>();
    holder->NoteWrite();
}

static void test_write_barrier_covers_old_to_young_edge(Runtime& rt) {
    Context& ctx = rt.GetContext();
    Holder* holder = make_old_holder(ctx, rt);
    give_young_child(holder);
    scrub_stack();
    Collector::collect_minor();
    CHECK(g_counted_alive == 1);

    holder->child = nullptr;
    Set(ctx, Value(ctx.get_global_object()), "oldHolder", Null());
    holder = nullptr;
    collect(rt);
    CHECK(g_counted_alive == 0);
    CHECK(g_holder_alive == 0);
}

// ---- Cast ------------------------------------------------------------------------

struct Figure : DOMObject {};
struct Round : Figure {
    using Parent = Figure;
};
struct Other : DOMObject {};

static void test_cast(Runtime& rt) {
    (void)rt;
    Round* round_one = Heap::Allocate<Round>();
    Figure* figure = Heap::Allocate<Figure>();
    Value c = FromObject(round_one);
    Value s = FromObject(figure);

    CHECK(DOMObject::Cast<Round>(c) == round_one);
    CHECK(DOMObject::Cast<Figure>(c) == static_cast<Figure*>(round_one));
    CHECK(DOMObject::Cast<DOMObject>(c) == static_cast<DOMObject*>(round_one));
    CHECK(DOMObject::Cast<Figure>(s) == figure);
    CHECK(DOMObject::Cast<Round>(s) == nullptr);
    CHECK(DOMObject::Cast<Other>(c) == nullptr);
    CHECK(DOMObject::Cast<Round>(Undefined()) == nullptr);
    CHECK(DOMObject::Cast<Round>(FromUint32(3)) == nullptr);
    CHECK(DOMObject::Cast<Round>(FromObject(rt.GetContext().get_global_object())) == nullptr);
}

// ---- A class exposed to script ----------------------------------------------------

static Object* g_counter_proto = nullptr;
static Object* g_counter_iterator_proto = nullptr;
static int g_counters_alive = 0;

struct Counter : DOMObject {
    uint32_t n = 0;
    Counter() { g_counters_alive++; }
    ~Counter() { g_counters_alive--; }
};

struct CounterIterator : DOMObject {
    uint32_t i = 0, end = 0;
};

static Counter* this_counter(Context& ctx, const Value& thisValue) {
    Counter* c = DOMObject::Cast<Counter>(thisValue);
    if (!c) ThrowTypeError(ctx, "Illegal invocation");
    return c;
}

static Value counter_construct(Context& ctx, Value, Args args, Value newTarget) {
    if (IsUndefined(newTarget)) {
        ThrowTypeError(ctx, "Failed to construct 'Counter': Please use the 'new' operator");
        return Undefined();
    }
    uint32_t start = 0;
    if (!args.empty()) {
        start = ToUint32(ctx, args[0]);
        if (HasException(ctx)) return Undefined();
    }
    Object* proto = PrototypeFromNewTarget(ctx, newTarget);
    if (HasException(ctx)) return Undefined();
    Counter* c = Heap::Allocate<Counter>();
    c->n = start;
    c->initialize_prototype(proto ? proto : g_counter_proto);
    return FromObject(c);
}

static Value counter_inc(Context& ctx, Value thisValue, Args, Value) {
    Counter* c = this_counter(ctx, thisValue);
    if (!c) return Undefined();
    return FromUint32(++c->n);
}

static Value counter_get_value(Context& ctx, Value thisValue, Args, Value) {
    Counter* c = this_counter(ctx, thisValue);
    return c ? FromUint32(c->n) : Undefined();
}

static Value counter_set_value(Context& ctx, Value thisValue, Args args, Value) {
    Counter* c = this_counter(ctx, thisValue);
    if (!c) return Undefined();
    uint32_t v = ToUint32(ctx, args.empty() ? Undefined() : args[0]);
    if (HasException(ctx)) return Undefined();
    c->n = v;
    return Undefined();
}

static Value counter_get_kind(Context&, Value, Args, Value) {
    return Undefined();
}

static Value counter_for_each(Context& ctx, Value thisValue, Args args, Value) {
    Counter* c = this_counter(ctx, thisValue);
    if (!c) return Undefined();
    if (args.empty() || !IsCallable(args[0])) {
        ThrowTypeError(ctx, "callback is not a function");
        return Undefined();
    }
    for (uint32_t i = 0; i < c->n; i++) {
        Value arg = FromUint32(i);
        Call(ctx, args[0], Undefined(), Args(&arg, 1));
        if (HasException(ctx)) return Undefined();
    }
    return Undefined();
}

static Value counter_echo(Context& ctx, Value, Args args, Value) {
    std::string s = ToUsvUtf8(ctx, args.empty() ? Undefined() : args[0]);
    if (HasException(ctx)) return Undefined();
    return FromUtf8(ctx, s);
}

static Value counter_from_utf8(Context& ctx, Value, Args, Value) {
    return FromUtf8(ctx, std::string_view("ok\xFF""\xC3\xA9", 5));
}

static Value counter_range(Context& ctx, Value, Args args, Value) {
    uint32_t n = ToUint32(ctx, args.empty() ? Undefined() : args[0]);
    if (HasException(ctx)) return Undefined();
    Value array = NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) ArrayPush(ctx, array, FromUint32(i));
    return array;
}

static Value counter_keys_of(Context& ctx, Value, Args args, Value) {
    if (args.empty()) return Undefined();
    std::vector<std::string> keys = OwnKeys(ctx, args[0]);
    if (HasException(ctx)) return Undefined();
    Value array = NewArray(ctx);
    for (const std::string& k : keys) ArrayPush(ctx, array, FromUtf8(ctx, k));
    return array;
}

static Value counter_get_prop(Context& ctx, Value, Args args, Value) {
    if (args.size() < 2) return Undefined();
    return Get(ctx, args[0], ToUsvUtf8(ctx, args[1]));
}

static Value counter_pending(Context& ctx, Value, Args, Value) {
    PromiseCapability cap = NewPromiseCapability(ctx);
    if (HasException(ctx)) return Undefined();
    Value global(ctx.get_global_object());
    Set(ctx, global, "__resolve", cap.resolve);
    Set(ctx, global, "__reject", cap.reject);
    return cap.promise;
}

static Value counter_iterator(Context& ctx, Value thisValue, Args, Value) {
    Counter* c = this_counter(ctx, thisValue);
    if (!c) return Undefined();
    CounterIterator* it = Heap::Allocate<CounterIterator>();
    it->end = c->n;
    it->initialize_prototype(g_counter_iterator_proto);
    return FromObject(it);
}

static Value counter_iterator_next(Context& ctx, Value thisValue, Args, Value) {
    CounterIterator* it = DOMObject::Cast<CounterIterator>(thisValue);
    if (!it) {
        ThrowTypeError(ctx, "Illegal invocation");
        return Undefined();
    }
    if (it->i >= it->end) return MakeIterResult(ctx, Undefined(), true);
    return MakeIterResult(ctx, FromUint32(it->i++), false);
}

static void define_counter(Runtime& rt) {
    Context& ctx = rt.GetContext();

    ClassRef counter = DefineClass(ctx, "Counter", counter_construct, 1);
    g_counter_proto = counter.prototype;
    DefineMethod(counter.prototype, "inc", counter_inc, 0);
    DefineMethod(counter.prototype, "forEach", counter_for_each, 1);
    DefineAccessor(counter.prototype, "value", counter_get_value, counter_set_value);
    DefineAccessor(counter.prototype, "readOnly", counter_get_value, nullptr);
    DefineStaticMethod(counter.constructor, "echo", counter_echo, 1);
    DefineStaticMethod(counter.constructor, "fromUtf8", counter_from_utf8, 0);
    DefineStaticMethod(counter.constructor, "range", counter_range, 1);
    DefineStaticMethod(counter.constructor, "keysOf", counter_keys_of, 1);
    DefineStaticMethod(counter.constructor, "getProp", counter_get_prop, 2);
    DefineStaticMethod(counter.constructor, "pending", counter_pending, 0);
    // Defined through the same call as any method; its key is the well-known
    // symbol's property key.
    ClassRef iterator = DefineClass(ctx, "CounterIterator", counter_construct, 0, GetIteratorPrototype(ctx));
    g_counter_iterator_proto = iterator.prototype;
    DefineMethod(iterator.prototype, "next", counter_iterator_next, 0);
    DefineToStringTag(iterator.prototype, "Counter Iterator");
    (void)counter_get_kind;
    DefineGlobal(ctx, "Counter", counter.constructor);

    // [Symbol.iterator] is installed from script: the embedding surface has no
    // symbol-keyed definitions, and this is what a Web IDL iterable<> needs.
    DefineMethod(counter.prototype, "__values", counter_iterator, 0);
    Runtime::Result r = rt.Evaluate("Counter.prototype[Symbol.iterator] = Counter.prototype.__values;"
                                    "delete Counter.prototype.__values; 'ok'");
    CHECK(r.ok);
}

// Evaluates an expression by assigning it to a global and reading that back.
static std::string js(Runtime& rt, const char* source) {
    Context& ctx = rt.GetContext();
    Runtime::Result r = rt.Evaluate(std::string("globalThis.__result = (") + source + ");");
    if (!r.ok) return "<error: " + r.error + ">";
    Value v = Get(ctx, Value(ctx.get_global_object()), "__result");
    return v.to_string();
}

#define EXPECT_JS(source, expected)                                                         \
    do {                                                                                    \
        std::string got = js(rt, source);                                                   \
        g_checks++;                                                                         \
        if (got != (expected)) {                                                            \
            g_failures++;                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n  expected: %s\n  got:      %s\n", __FILE__, \
                         __LINE__, source, std::string(expected).c_str(), got.c_str());     \
        }                                                                                   \
    } while (0)

static void test_class_from_script(Runtime& rt) {
    define_counter(rt);

    EXPECT_JS("new Counter(5).inc()", "6");
    EXPECT_JS("new Counter(5).value", "5");
    EXPECT_JS("Object.prototype.toString.call(new Counter())", "[object Counter]");
    EXPECT_JS("new Counter() instanceof Counter", "true");
    EXPECT_JS("Counter.length", "1");
    EXPECT_JS("Counter.name", "Counter");
    EXPECT_JS("Counter.prototype.constructor === Counter", "true");

    // Brand check, and calling without `new`.
    EXPECT_JS("(() => { try { Counter(); } catch (e) { return e instanceof TypeError; } })()", "true");
    EXPECT_JS("(() => { try { Counter.prototype.inc.call({}); } catch (e) { return e instanceof TypeError; } })()",
              "true");

    // Accessors: a setter that converts, and an attribute with none.
    EXPECT_JS("(() => { const c = new Counter(1); c.value = '9'; return c.value; })()", "9");
    EXPECT_JS("(() => { const c = new Counter(4); c.readOnly = 99; return c.readOnly; })()", "4");
    EXPECT_JS("Object.getOwnPropertyDescriptor(Counter.prototype, 'value').enumerable", "true");
    EXPECT_JS("typeof Object.getOwnPropertyDescriptor(Counter.prototype, 'readOnly').set", "undefined");

    // Subclassing picks the subclass's prototype through newTarget.
    EXPECT_JS("(() => { class Sub extends Counter { double() { return this.inc() * 2; } }"
              "const s = new Sub(2); return [s instanceof Sub, s instanceof Counter, s.double()].join(); })()",
              "true,true,6");

    // Calling back into script, and an exception from the callback surviving.
    EXPECT_JS("(() => { const acc = []; new Counter(3).forEach(i => acc.push(i)); return acc.join(); })()",
              "0,1,2");
    EXPECT_JS("(() => { try { new Counter(2).forEach(() => { throw new RangeError('boom'); }); }"
              "catch (e) { return e.message + ':' + (e instanceof RangeError); } })()",
              "boom:true");

    // Iteration protocol.
    EXPECT_JS("[...new Counter(3)].join()", "0,1,2");
    EXPECT_JS("(() => { let s = 0; for (const x of new Counter(4)) s += x; return s; })()", "6");
    EXPECT_JS("Object.getPrototypeOf(Object.getPrototypeOf(new Counter()[Symbol.iterator]())) === Iterator.prototype",
              "true");

    // Arrays and property access from the native side.
    EXPECT_JS("Counter.range(4).join()", "0,1,2,3");
    EXPECT_JS("Array.isArray(Counter.range(2))", "true");
    EXPECT_JS("Counter.keysOf({b: 1, a: 2, [Symbol('s')]: 3}).join()", "b,a");
    EXPECT_JS("Counter.getProp({get x() { return 41 + 1; }}, 'x')", "42");
    EXPECT_JS("(() => { try { Counter.getProp({get x() { throw new TypeError('no'); }}, 'x'); }"
              "catch (e) { return e.message; } })()",
              "no");

    // Strings: lone surrogates become U+FFFD, pairs survive, bad UTF-8 is repaired.
    EXPECT_JS(R"(Counter.echo('a\uD800b'))", "a\xEF\xBF\xBD" "b");
    EXPECT_JS(R"(Counter.echo('\uDC00'))", "\xEF\xBF\xBD");
    EXPECT_JS(R"(Counter.echo('x😀y'))", "x\xF0\x9F\x98\x80y");
    EXPECT_JS(R"(Counter.echo('\uD83D' + '\uDE00'))", "\xF0\x9F\x98\x80");
    EXPECT_JS(R"(Counter.echo('\uDE00\uD83D'))", "\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT_JS("Counter.echo(123)", "123");
    EXPECT_JS("(() => { try { Counter.echo(Symbol()); } catch (e) { return e instanceof TypeError; } })()", "true");
    EXPECT_JS("Counter.fromUtf8()", "ok\xEF\xBF\xBD\xC3\xA9");
    EXPECT_JS("Counter.echo({toString() { return 'obj'; }})", "obj");
}

static void test_to_uint32(Runtime& rt) {
    Context& ctx = rt.GetContext();
    CHECK(ToUint32(ctx, Value(4294967297.0)) == 1);
    CHECK(ToUint32(ctx, Value(-1.0)) == 4294967295u);
    CHECK(ToUint32(ctx, Value(3.9)) == 3);
    CHECK(ToUint32(ctx, Value::nan()) == 0);
    CHECK(ToUint32(ctx, Value::positive_infinity()) == 0);
    CHECK(ToUint32(ctx, Undefined()) == 0);
}

// Objects the script makes in bulk go through real safepoint collections, not
// just the explicit ones above.
static void test_script_driven_gc(Runtime& rt) {
    EXPECT_JS("(() => { let last = 0; for (let i = 0; i < 40000; i++) last = new Counter(i).inc(); return last; })()",
              "40000");
    EXPECT_JS("(() => { const keep = []; for (let i = 0; i < 2000; i++) keep.push(new Counter(i));"
              "for (let i = 0; i < 20000; i++) new Counter(i);"
              "return keep.map(c => c.value).reduce((a, b) => a + b, 0); })()",
              "1999000");
    collect(rt);
    CHECK(g_counters_alive < 2100);
}

static void settle(Runtime& rt, const char* which, const Value& with) {
    Context& ctx = rt.GetContext();
    Value fn = Get(ctx, Value(ctx.get_global_object()), which);
    Call(ctx, fn, Undefined(), Args(&with, 1));
    CHECK(!HasException(ctx));
}

static void test_promises_and_microtasks(Runtime& rt) {
    Context& ctx = rt.GetContext();

    // A job queued by script runs inside Evaluate: the host did not have to ask.
    EXPECT_JS("(() => { globalThis.queued = 0; Promise.resolve().then(() => { globalThis.queued = 1; });"
              "return 'scheduled'; })()", "scheduled");
    EXPECT_JS("globalThis.queued", "1");

    // A promise the host settles from outside script: nothing runs until the
    // host takes its microtask checkpoint.
    CHECK(rt.Evaluate("globalThis.got = 'none'; Counter.pending().then(v => { globalThis.got = v; });").ok);
    EXPECT_JS("globalThis.got", "none");
    settle(rt, "__resolve", FromUint32(42));
    EXPECT_JS("globalThis.got", "none");
    rt.PerformMicrotaskCheckpoint();
    EXPECT_JS("globalThis.got", "42");

    // resolve adopts a thenable rather than wrapping it.
    CHECK(rt.Evaluate("globalThis.adopted = 'none'; globalThis.thenable = { then(done) { done('via then'); } };"
                      "Counter.pending().then(v => { globalThis.adopted = v; });").ok);
    settle(rt, "__resolve", Get(ctx, Value(ctx.get_global_object()), "thenable"));
    rt.PerformMicrotaskCheckpoint();
    EXPECT_JS("globalThis.adopted", "via then");

    CHECK(rt.Evaluate("globalThis.why = 'none'; Counter.pending().catch(e => { globalThis.why = e; });").ok);
    settle(rt, "__reject", FromUtf8(ctx, "because"));
    rt.PerformMicrotaskCheckpoint();
    EXPECT_JS("globalThis.why", "because");
}

static void test_host_drives_timers(Runtime& rt) {
    CHECK(!rt.NextTimerDelayMs().has_value());
    CHECK(!rt.RunDueTimers());

    // Evaluate returns without sleeping through or firing the timer.
    CHECK(rt.Evaluate("globalThis.tick = 0; setTimeout(() => { globalThis.tick = 1; }, 0);").ok);
    EXPECT_JS("globalThis.tick", "0");
    CHECK(rt.NextTimerDelayMs().has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(rt.RunDueTimers());
    EXPECT_JS("globalThis.tick", "1");
    CHECK(!rt.NextTimerDelayMs().has_value());

    // A timer that is not due is left alone, and the host is told how long to wait.
    CHECK(rt.Evaluate("setTimeout(() => { globalThis.tick = 2; }, 60000);").ok);
    CHECK(!rt.RunDueTimers());
    EXPECT_JS("globalThis.tick", "1");
    std::optional<int64_t> wait = rt.NextTimerDelayMs();
    CHECK(wait.has_value() && *wait > 50000 && *wait <= 60000);
}

static void test_external_allocation_requests_collection() {
    Heap::clear_gc_request();
    CHECK(!Heap::gc_requested());
    ReportExternalAllocation(size_t(1) << 30);
    CHECK(Heap::gc_requested());
    Heap::clear_gc_request();
}

int main() {
    std::unique_ptr<Runtime> rt = Runtime::Create();
    if (!rt) {
        std::fprintf(stderr, "Runtime::Create failed\n");
        return 1;
    }

    test_destructor_runs_on_sweep(*rt);
    test_visit_keeps_children_alive(*rt);
    test_write_barrier_covers_old_to_young_edge(*rt);
    test_cast(*rt);
    test_to_uint32(*rt);
    test_class_from_script(*rt);
    test_script_driven_gc(*rt);
    test_promises_and_microtasks(*rt);
    test_host_drives_timers(*rt);
    test_external_allocation_requests_collection();

    std::printf("embed-test: %d checks, %d failed\n", g_checks, g_failures);
    // The heap is immortal by design; skip the static destructors that would
    // run against it.
    std::fflush(stdout);
    std::_Exit(g_failures == 0 ? 0 : 1);
}
