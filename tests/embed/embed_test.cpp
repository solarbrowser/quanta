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
#include <functional>
#include <map>
#include <set>
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

static Value counter_iterator_method(Context& ctx, Value, Args args, Value) {
    return GetIteratorMethod(ctx, args.empty() ? Undefined() : args[0]);
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
    DefineStaticMethod(counter.constructor, "iteratorMethod", counter_iterator_method, 1);
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

    // Telling a sequence from a record: the iterator method, or nothing.
    EXPECT_JS("typeof Counter.iteratorMethod([['a', 'b']])", "function");
    EXPECT_JS("Counter.iteratorMethod([1]) === Array.prototype[Symbol.iterator]", "true");
    EXPECT_JS("typeof Counter.iteratorMethod(new Map())", "function");
    EXPECT_JS("typeof Counter.iteratorMethod('abc')", "function");
    EXPECT_JS("typeof Counter.iteratorMethod(new Counter(1))", "function");
    EXPECT_JS("typeof Counter.iteratorMethod({a: 'b'})", "undefined");
    EXPECT_JS("typeof Counter.iteratorMethod({[Symbol.iterator]: undefined})", "undefined");
    EXPECT_JS("typeof Counter.iteratorMethod({[Symbol.iterator]: null})", "undefined");
    EXPECT_JS("typeof Counter.iteratorMethod(5)", "undefined");
    EXPECT_JS("(() => { try { Counter.iteratorMethod({[Symbol.iterator]: 5}); } catch (e) { return e instanceof TypeError; } })()",
              "true");
    EXPECT_JS("(() => { try { Counter.iteratorMethod(null); } catch (e) { return e instanceof TypeError; } })()", "true");
    EXPECT_JS("(() => { try { Counter.iteratorMethod({get [Symbol.iterator]() { throw new RangeError('x'); }}); }"
              "catch (e) { return e instanceof RangeError; } })()",
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

// A Runtime made after another was destroyed must start clean: nothing of the
// first one's realm may be reachable from thread-wide state, and a collection
// must never trace what the first one left behind. The script is shaped like a
// conformance suite -- many closures made in nested loops, each compiled on
// its first call, with enough allocation between them for collections to land
// in the middle -- because a chunk compiled after the collector last looked at
// its function once had its constants swept while the chunk lived on.
static void test_sequential_runtimes() {
    const char* script = R"JS(
      // Collections that land after the whole script is parsed and before any of
      // its closures has run: every function's executable is seen by the
      // collector with no compiled body yet.
      for (let k = 0; k < 3000; k++) { const warm = { a: k, b: 'p' + k }; }
      const failures = [];
      function eq(actual, expected, what) {
        if (actual !== expected) failures.push(what + ': ' + actual + ' vs ' + expected);
      }
      for (const scheme of ['https', 'wpt++']) {
        for (let i = 0; i < 6; i++) {
          for (const [type, part] of [['leading', 'a'], ['middle', 'b'], ['trailing', 'c']]) {
            const run = () => {
              const expected = scheme + '/' + type + '/' + part;
              eq([scheme, type, part].join('/'), expected, 'join ' + type);
              eq(expected + ':8000', scheme + '/' + type + '/' + part + ':8000', 'port');
              eq(String(i), '' + i, 'number');
            };
            run();
            const junk = [];
            for (let j = 0; j < 60; j++) junk.push({ k: 'x' + j, v: [j] });
          }
        }
      }
      globalThis.failureCount = failures.length;
      globalThis.firstFailure = failures[0] || '';
    )JS";

    constexpr int kRuntimes = 12;
    int failed_runtimes = 0;
    std::string first;
    for (int i = 0; i < kRuntimes; i++) {
        std::unique_ptr<Runtime> rt = Runtime::Create();
        if (!rt) { failed_runtimes++; continue; }
        Runtime::Result r = rt->Evaluate(script);
        Context& ctx = rt->GetContext();
        Value count = Get(ctx, Value(ctx.get_global_object()), "failureCount");
        if (!r.ok || !count.is_number() || count.as_number() != 0) {
            failed_runtimes++;
            if (first.empty()) first = "run " + std::to_string(i) + ": " + (r.ok ? Get(ctx, Value(ctx.get_global_object()), "firstFailure").to_string() : r.error);
        }
    }
    g_checks++;
    if (failed_runtimes != 0) {
        g_failures++;
        std::fprintf(stderr, "FAIL sequential runtimes: %d of %d went wrong, first: %s\n", failed_runtimes, kRuntimes, first.c_str());
    }
}


// A script has no completion value, so a result is read back from a global.
static Value eval_in(Embed::Realm& realm, const std::string& expr) {
    realm.Evaluate("globalThis.__r = (" + expr + ");");
    Context& ctx = realm.GetContext();
    return Get(ctx, Value(ctx.get_global_object()), "__r");
}

static Value global_of(Embed::Realm& realm, const char* name) {
    Context& ctx = realm.GetContext();
    return Get(ctx, Value(ctx.get_global_object()), name);
}

static void set_global(Embed::Realm& realm, const char* name, const Value& value) {
    Context& ctx = realm.GetContext();
    Set(ctx, Value(ctx.get_global_object()), name, value);
}

// Realms that share an Isolate live at once, each with its own intrinsics, and one can
// be destroyed while another still holds closures it made.
static void test_realms() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    CHECK(isolate != nullptr);
    CHECK(Embed::Isolate::Create() == nullptr);

    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    std::unique_ptr<Embed::Realm> b = isolate->CreateRealm();
    CHECK(a && b);

    CHECK(eval_in(*a, "Array.prototype").as_object() != eval_in(*b, "Array.prototype").as_object());
    CHECK(eval_in(*a, "[1, 2].map(x => x + 1).length").as_number() == 2);
    CHECK(eval_in(*b, "'abc'.constructor === String").as_boolean());

    // A closure of b, held by a: it runs in b, and what it makes is b's.
    b->Evaluate("globalThis.f = (function () { var n = 40; return function () { return [n, 2].map(x => x + 1); }; })();"
                "globalThis.bArray = Array; globalThis.bTypeError = TypeError;");
    set_global(*a, "g", global_of(*b, "f"));
    set_global(*a, "bArray", global_of(*b, "bArray"));
    set_global(*a, "bTypeError", global_of(*b, "bTypeError"));
    CHECK(eval_in(*a, "g().length + g()[0]").as_number() == 43);
    CHECK(eval_in(*a, "Array.isArray(g())").as_boolean());
    CHECK(eval_in(*a, "!(g() instanceof Array)").as_boolean());
    CHECK(eval_in(*a, "g() instanceof bArray").as_boolean());
    // An error a function of b throws is b's, whoever called it.
    b->Evaluate("globalThis.thrower = function () { null.x; };");
    set_global(*a, "thrower", global_of(*b, "thrower"));
    CHECK(eval_in(*a, "(function () { try { thrower(); } catch (e) { return e instanceof bTypeError && !(e instanceof TypeError); } })()").as_boolean());

    // Patching one realm's iteration leaves the other's alone.
    CHECK(eval_in(*a, "(function () { Array.prototype[Symbol.iterator] = function* () { yield 99; }; return [...[1, 2, 3]].join(); })()").to_string() == "99");
    CHECK(eval_in(*b, "[...[1, 2, 3]].join()").to_string() == "1,2,3");
    CHECK(eval_in(*b, "[1, 2, 3].map(x => x * 2).join()").to_string() == "2,4,6");

    // b's timer goes with b.
    b->Evaluate("setTimeout(function () { globalThis.timerRan = true; }, 0);");
    CHECK(isolate->NextTimerDelayMs().has_value());
    b.reset();
    CHECK(!isolate->NextTimerDelayMs().has_value());
    isolate->RunDueTimers();
    isolate->PerformMicrotaskCheckpoint();

    isolate->CollectGarbage();
    isolate->CollectGarbage();
    CHECK(eval_in(*a, "g().length + g()[0]").as_number() == 43);
    CHECK(eval_in(*a, "(function () { try { thrower(); } catch (e) { return e instanceof bTypeError; } })()").as_boolean());

    // Once nothing of b's is held, it is freed, and a goes on.
    a->Evaluate("g = bArray = bTypeError = thrower = undefined;");
    isolate->CollectGarbage();
    isolate->CollectGarbage();
    CHECK(eval_in(*a, "[1, 2, 3].map(x => x * 2).length").as_number() == 3);

    // Realms come and go in one Isolate without disturbing the one that stays.
    for (int i = 0; i < 6; i++) {
        std::unique_ptr<Embed::Realm> c = isolate->CreateRealm();
        CHECK(c != nullptr);
        c->Evaluate("globalThis.keep = [1, 2, 3].map(x => x + 1);");
        set_global(*a, "k", global_of(*c, "keep"));
        c.reset();
        isolate->CollectGarbage();
        CHECK(eval_in(*a, "k.length").as_number() == 3);
    }

    // The last realm can go, and a new one take its place.
    a.reset();
    isolate->CollectGarbage();
    std::unique_ptr<Embed::Realm> d = isolate->CreateRealm();
    CHECK(d != nullptr);
    CHECK(eval_in(*d, "[1, 2, 3].map(x => x * 2).length").as_number() == 3);

    // An Isolate that goes first takes the realms still alive with it.
    std::unique_ptr<Embed::Realm> e = isolate->CreateRealm();
    CHECK(e != nullptr);
    isolate.reset();
    d.reset();
    e.reset();

    // And a new Isolate may follow.
    std::unique_ptr<Embed::Isolate> next = Embed::Isolate::Create();
    CHECK(next != nullptr);
    std::unique_ptr<Embed::Realm> f = next->CreateRealm();
    CHECK(eval_in(*f, "1 + 1").as_number() == 2);
}

static int g_realm_key;

static Value ProbeRealm(Context& ctx, Value, Args, Value) {
    return Value(static_cast<double>(reinterpret_cast<intptr_t>(GetRealmData(ctx, &g_realm_key))));
}

static void define_probe(Embed::Realm& realm, intptr_t tag) {
    Context& ctx = realm.GetContext();
    SetRealmData(ctx, &g_realm_key, reinterpret_cast<void*>(tag));
    ClassRef probe = DefineClass(ctx, "Probe", [](Context&, Value, Args, Value) { return Undefined(); }, 0);
    DefineStaticMethod(probe.constructor, "get", ProbeRealm, 0);
    DefineGlobal(ctx, "Probe", probe.constructor);
}

// What a host keeps per realm is told apart by the realm the native runs in.
static void test_realm_data() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    std::unique_ptr<Embed::Realm> b = isolate->CreateRealm();
    define_probe(*a, 1);
    define_probe(*b, 2);
    CHECK(eval_in(*a, "Probe.get()").as_number() == 1);
    CHECK(eval_in(*b, "Probe.get()").as_number() == 2);
    // b's native, called by a's script, still answers for b.
    set_global(*a, "BProbe", global_of(*b, "Probe"));
    CHECK(eval_in(*a, "BProbe.get()").as_number() == 2);
    CHECK(eval_in(*a, "Probe.get()").as_number() == 1);
    // And through a callback b's native makes into a function of a's.
    set_global(*b, "AProbe", global_of(*a, "Probe"));
    CHECK(eval_in(*b, "AProbe.get()").as_number() == 1);
}

static Value ProbeMethod(Context& ctx, Value, Args, Value) {
    return Value(static_cast<double>(reinterpret_cast<intptr_t>(GetRealmData(ctx, &g_realm_key))) + 100);
}

// What a destroyed realm defined lives as long as something of it is held: the class
// (its prototype and constructor) and what its natives find per realm.
static void test_destroyed_realm_class() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    std::unique_ptr<Embed::Realm> b = isolate->CreateRealm();

    Context& cb = b->GetContext();
    SetRealmData(cb, &g_realm_key, reinterpret_cast<void*>(7));
    ClassRef klass = DefineClass(cb, "Kept", [](Context&, Value, Args, Value) { return Undefined(); }, 0);
    DefineMethod(klass.prototype, "tag", ProbeMethod, 0);
    DefineStaticMethod(klass.constructor, "make", [](Context&, Value, Args, Value) { return Value(1.0); }, 0);
    DefineGlobal(cb, "Kept", klass.constructor);

    // a holds one instance and nothing else of b's.
    b->Evaluate("globalThis.inst = Object.create(Kept.prototype);");
    set_global(*a, "inst", global_of(*b, "inst"));
    b.reset();

    for (int i = 0; i < 3; i++) {
        isolate->CollectGarbage();
        CHECK(eval_in(*a, "inst.tag()").as_number() == 107);
        CHECK(eval_in(*a, "typeof inst.constructor.make").to_string() == "function");
        CHECK(eval_in(*a, "inst.constructor.make()").as_number() == 1);
        CHECK(eval_in(*a, "Object.getPrototypeOf(inst) === inst.constructor.prototype").as_boolean());
        CHECK(eval_in(*a, "inst instanceof inst.constructor").as_boolean());
        CHECK(eval_in(*a, "Object.prototype.toString.call(inst)").to_string() == "[object Kept]");
        a->Evaluate("for (let i = 0; i < 2000; i++) { [i, i + 1].map(x => x * 2); }");
    }

    // Once the instance goes, so does the rest, and a is unaffected.
    a->Evaluate("inst = undefined;");
    isolate->CollectGarbage();
    isolate->CollectGarbage();
    CHECK(eval_in(*a, "[1, 2, 3].map(x => x * 2).length").as_number() == 3);
}

// A Persistent keeps a pending promise and the objects it will be settled with alive
// across collections the host does not control.
static void test_persistent() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Context& ctx = realm->GetContext();

    PromiseCapability cap = NewPromiseCapability(ctx);
    Persistent promise(ctx, cap.promise);
    Persistent resolve(ctx, cap.resolve);
    Persistent payload;
    {
        realm->Evaluate("globalThis.__mk = { answer: [1, 2, 3].map(x => x * 14) };");
        payload = Persistent(ctx, Get(ctx, Value(ctx.get_global_object()), "__mk"));
        realm->Evaluate("delete globalThis.__mk;");
    }
    cap = PromiseCapability();
    CHECK(!promise.IsEmpty());

    // Churn: plenty of garbage and several full collections while only the Persistents hold them.
    for (int i = 0; i < 4; i++) {
        realm->Evaluate("for (let i = 0; i < 3000; i++) { ({ v: [i, 'x' + i] }); }");
        isolate->CollectGarbage();
    }

    Value answer = Get(ctx, payload.Get(), "answer");
    CHECK(Get(ctx, answer, "length").as_number() == 3);
    CHECK(Get(ctx, answer, "2").as_number() == 42);
    CHECK(!HasException(ctx));

    set_global(*realm, "settled", Value(0.0));
    set_global(*realm, "thePromise", promise.Get());
    realm->Evaluate("thePromise.then(v => { globalThis.settled = v.answer[0]; });");
    Value payload_value = payload.Get();
    Call(ctx, resolve.Get(), Undefined(), Args(&payload_value, 1));
    isolate->PerformMicrotaskCheckpoint();
    CHECK(global_of(*realm, "settled").as_number() == 14);

    promise.Reset();
    CHECK(promise.IsEmpty());
    CHECK(promise.Get().is_undefined());
    resolve.Reset();
    payload.Reset();
    isolate->CollectGarbage();
}

static void test_byte_buffers() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Context& ctx = realm->GetContext();

    const uint8_t data[] = {1, 2, 3, 250, 251};
    Value array = NewUint8Array(ctx, std::span<const uint8_t>(data, 5));
    CHECK(!HasException(ctx));
    set_global(*realm, "u8", array);
    CHECK(eval_in(*realm, "u8 instanceof Uint8Array && u8.length === 5 && u8[3] === 250").as_boolean());
    auto bytes = BytesOf(array);
    CHECK(bytes && bytes->size() == 5 && (*bytes)[4] == 251);

    Value empty = NewUint8Array(ctx, {});
    CHECK(BytesOf(empty) && BytesOf(empty)->size() == 0);

    // Script writes are visible through the span, and a view's window is its own bytes.
    realm->Evaluate("u8[0] = 9; globalThis.win = u8.subarray(1, 3); globalThis.dv = new DataView(u8.buffer, 2, 2);"
                    "globalThis.buf = u8.buffer; globalThis.i16 = new Int16Array(new ArrayBuffer(8), 2, 2);");
    CHECK((*BytesOf(array))[0] == 9);
    auto win = BytesOf(global_of(*realm, "win"));
    CHECK(win && win->size() == 2 && (*win)[0] == 2 && (*win)[1] == 3);
    auto dv = BytesOf(global_of(*realm, "dv"));
    CHECK(dv && dv->size() == 2 && (*dv)[0] == 3);
    auto whole = BytesOf(global_of(*realm, "buf"));
    CHECK(whole && whole->size() == 5);
    auto i16 = BytesOf(global_of(*realm, "i16"));
    CHECK(i16 && i16->size() == 4);

    // Not bytes, or no longer any.
    CHECK(!BytesOf(Value(1.0)));
    CHECK(!BytesOf(eval_in(*realm, "({})")));
    realm->Evaluate("globalThis.moved = buf.transfer();");
    CHECK(!BytesOf(global_of(*realm, "buf")));
    CHECK(!BytesOf(array));
}

// The order a record conversion observes on a Proxy: [[OwnPropertyKeys]], then for each key
// [[GetOwnProperty]] and, if it is enumerable, [[Get]].
static void test_record_order() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Context& ctx = realm->GetContext();

    realm->Evaluate(
        "globalThis.log = [];"
        "const sym = Symbol('s');"
        "const target = { a: 1, b: 2, [sym]: 3 };"
        "Object.defineProperty(target, 'hidden', { value: 4, enumerable: false });"
        "globalThis.sym = sym;"
        "globalThis.proxy = new Proxy(target, {"
        "  ownKeys(t) { log.push('ownKeys'); return Reflect.ownKeys(t); },"
        "  getOwnPropertyDescriptor(t, k) { log.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },"
        "  get(t, k, r) { log.push('get:' + String(k)); return Reflect.get(t, k, r); } });");
    Value proxy = global_of(*realm, "proxy");

    ValueList keys = OwnPropertyKeys(ctx, proxy);
    CHECK(!HasException(ctx));
    CHECK(keys.size() == 4);
    double sum = 0;
    for (const Value& key : keys) {
        if (!GetOwnEnumerable(ctx, proxy, key)) continue;
        Value v = Get(ctx, proxy, key);
        CHECK(!HasException(ctx));
        sum += v.as_number();
    }
    CHECK(sum == 6);
    CHECK(eval_in(*realm, "log.join()").to_string() ==
          "ownKeys,gopd:a,get:a,gopd:b,get:b,gopd:hidden,gopd:Symbol(s),get:Symbol(s)");

    // A trap that throws is reported through the flag.
    realm->Evaluate("globalThis.bad = new Proxy({}, { ownKeys() { throw new RangeError('no'); } });");
    ValueList none = OwnPropertyKeys(ctx, global_of(*realm, "bad"));
    CHECK(HasException(ctx) && none.size() == 0);
    ctx.clear_exception();

    // The iterator prototype is Iterator.prototype.
    CHECK(eval_in(*realm, "Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())) === Iterator.prototype").as_boolean());
}

static Value AddOne(Context& ctx, Value, Args args, Value newTarget) {
    if (!IsUndefined(newTarget)) {
        ThrowTypeError(ctx, "not a constructor");
        return Undefined();
    }
    return Value(ToUint32(ctx, args.empty() ? Undefined() : args[0]) + 1.0);
}

// A global function belongs to the realm it was defined in; a typed array's own buffer is born
// with that realm's ArrayBuffer.prototype; a Proxy over a Proxy answers the descriptor invariants
// from the inner one.
static void test_globals_buffers_nested_proxies() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    std::unique_ptr<Embed::Realm> b = isolate->CreateRealm();
    DefineGlobalFunction(a->GetContext(), "addOne", AddOne, 1);
    CHECK(eval_in(*a, "addOne(41)").as_number() == 42);
    CHECK(eval_in(*a, "typeof addOne + Object.getOwnPropertyDescriptor(globalThis, 'addOne').enumerable").to_string() == "functionfalse");
    CHECK(eval_in(*a, "(() => { try { new addOne(1); } catch (e) { return e instanceof TypeError; } })()").as_boolean());
    CHECK(eval_in(*b, "typeof addOne").to_string() == "undefined");

    const uint8_t data[] = {1, 2, 3};
    Value in_b = NewUint8Array(b->GetContext(), std::span<const uint8_t>(data, 3));
    set_global(*b, "u8", in_b);
    CHECK(eval_in(*b, "u8.buffer instanceof ArrayBuffer && Object.getPrototypeOf(u8.buffer) === ArrayBuffer.prototype").as_boolean());
    set_global(*a, "bu8", in_b);
    CHECK(eval_in(*a, "!(bu8.buffer instanceof ArrayBuffer) && Object.getPrototypeOf(bu8.buffer) !== ArrayBuffer.prototype").as_boolean());

    CHECK(eval_in(*a,
        "(() => { const t = {}; Object.defineProperty(t, 'x', { value: 1, configurable: false });"
        "const lie = new Proxy(new Proxy(t, {}), { getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } });"
        "try { Object.getOwnPropertyDescriptor(lie, 'x'); return false; } catch (e) { return e instanceof TypeError; } })()").as_boolean());
    CHECK(eval_in(*a,
        "(() => { const t = Object.preventExtensions({});"
        "const lie = new Proxy(new Proxy(t, {}), { getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } });"
        "try { Object.getOwnPropertyDescriptor(lie, 'x'); return false; } catch (e) { return e instanceof TypeError; } })()").as_boolean());
    CHECK(eval_in(*a, "JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy(new Proxy({ k: 1 }, {}), {}), 'k'))").to_string() ==
          "{\"value\":1,\"writable\":true,\"enumerable\":true,\"configurable\":true}");
}

// ---- Legacy platform objects ----------------------------------------------

static int g_items_key;
static int g_readonly_key;
static int g_overriding_key;

// Indexed and named properties, both writable, names not enumerable.
struct Items : DOMObject {
    std::vector<std::string> values;
    std::map<std::string, std::string> named;

    static bool IndexedGetter(Context& ctx, Items& t, uint32_t i, Value& out) {
        if (i >= t.values.size()) return false;
        out = FromUtf8(ctx, t.values[i]);
        return true;
    }
    static void IndexedSetter(Context& ctx, Items& t, uint32_t i, const Value& v) {
        std::string text = ToUsvUtf8(ctx, v);
        if (i < t.values.size()) t.values[i] = text;
        else if (i == t.values.size()) t.values.push_back(text);
    }
    static bool IndexedDeleter(Context&, Items& t, uint32_t i) {
        if (i + 1 != t.values.size()) return false;
        t.values.pop_back();
        return true;
    }
    static uint32_t IndexedLength(Context&, Items& t) { return static_cast<uint32_t>(t.values.size()); }
    static bool NamedGetter(Context& ctx, Items& t, const std::string& name, Value& out) {
        auto it = t.named.find(name);
        if (it == t.named.end()) return false;
        out = FromUtf8(ctx, it->second);
        return true;
    }
    static void NamedSetter(Context& ctx, Items& t, const std::string& name, const Value& v) {
        t.named[name] = ToUsvUtf8(ctx, v);
    }
    static bool NamedDeleter(Context&, Items& t, const std::string& name) { return t.named.erase(name) != 0; }
    static std::vector<std::string> NamedKeys(Context&, Items& t) {
        std::vector<std::string> names;
        for (auto& [name, value] : t.named) names.push_back(name);
        return names;
    }
    static constexpr bool LegacyUnenumerableNamedProperties = true;
};

// Getters only: what NodeList and HTMLCollection are.
struct ReadOnlyList : DOMObject {
    std::vector<std::string> values{"x", "y"};
    static bool IndexedGetter(Context& ctx, ReadOnlyList& t, uint32_t i, Value& out) {
        if (i >= t.values.size()) return false;
        out = FromUtf8(ctx, t.values[i]);
        return true;
    }
    static uint32_t IndexedLength(Context&, ReadOnlyList& t) { return static_cast<uint32_t>(t.values.size()); }
    static bool NamedGetter(Context& ctx, ReadOnlyList&, const std::string& name, Value& out) {
        if (name != "id" && name != "forEach") return false;
        out = FromUtf8(ctx, "named:" + name);
        return true;
    }
    static std::vector<std::string> NamedKeys(Context&, ReadOnlyList&) { return {"id", "forEach"}; }
};

struct Overriding : DOMObject {
    static bool NamedGetter(Context& ctx, Overriding&, const std::string& name, Value& out) {
        if (name != "toString" && name != "plain") return false;
        out = FromUtf8(ctx, "named:" + name);
        return true;
    }
    static constexpr bool LegacyOverrideBuiltIns = true;
};

template <class T>
static Value make_legacy(Context& ctx, const void* key) {
    T* object = Heap::Allocate<T>();
    if (Object* proto = static_cast<Object*>(GetRealmData(ctx, key))) object->initialize_prototype(proto);
    return FromObject(object);
}

static Value ItemsLength(Context&, Value thisValue, Args, Value) {
    Items* items = DOMObject::Cast<Items>(thisValue);
    return Value(items ? static_cast<double>(items->values.size()) : 0.0);
}
static Value ReadOnlyLength(Context&, Value thisValue, Args, Value) {
    ReadOnlyList* list = DOMObject::Cast<ReadOnlyList>(thisValue);
    return Value(list ? static_cast<double>(list->values.size()) : 0.0);
}

static void define_legacy(Embed::Realm& realm) {
    Context& ctx = realm.GetContext();
    auto noop = [](Context&, Value, Args, Value) { return Undefined(); };
    ClassRef items = DefineClass(ctx, "Items", noop, 0);
    DefineAccessor(items.prototype, "length", ItemsLength, nullptr);
    SetRealmData(ctx, &g_items_key, items.prototype);
    DefineGlobal(ctx, "Items", items.constructor);
    ClassRef readonly = DefineClass(ctx, "ReadOnlyList", noop, 0);
    DefineAccessor(readonly.prototype, "length", ReadOnlyLength, nullptr);
    SetRealmData(ctx, &g_readonly_key, readonly.prototype);
    DefineGlobal(ctx, "ReadOnlyList", readonly.constructor);
    ClassRef overriding = DefineClass(ctx, "Overriding", noop, 0);
    SetRealmData(ctx, &g_overriding_key, overriding.prototype);
    DefineGlobalFunction(ctx, "makeItems", [](Context& c, Value, Args, Value) {
        Value v = make_legacy<Items>(c, &g_items_key);
        Items* items = DOMObject::Cast<Items>(v);
        items->values = {"a", "b", "c"};
        items->named = {{"id", "x"}, {"title", "t"}};
        return v;
    }, 0);
    DefineGlobalFunction(ctx, "makeReadOnly", [](Context& c, Value, Args, Value) { return make_legacy<ReadOnlyList>(c, &g_readonly_key); }, 0);
    DefineGlobalFunction(ctx, "makeOverriding", [](Context& c, Value, Args, Value) { return make_legacy<Overriding>(c, &g_overriding_key); }, 0);
    DefineGlobalFunction(ctx, "addNamed", [](Context& c, Value, Args args, Value) {
        Items* items = DOMObject::Cast<Items>(args[0]);
        if (items) items->named[ToUsvUtf8(c, args[1])] = ToUsvUtf8(c, args[2]);
        return Undefined();
    }, 3);
}

static void test_legacy_platform_objects() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    define_legacy(*realm);
    Embed::Realm& r = *realm;
    auto js_str = [&](const char* expr) { return eval_in(r, expr).to_string(); };
    auto js_true = [&](const char* expr) { return eval_in(r, expr).as_boolean(); };

    r.Evaluate("globalThis.items = makeItems(); globalThis.ro = makeReadOnly(); globalThis.ov = makeOverriding();");

    // Reading.
    CHECK(js_str("items[0] + items[1] + items['2']") == "abc");
    CHECK(js_true("items[3] === undefined && items['01'] === undefined && items[-1] === undefined"));
    CHECK(js_str("items.id + items.title") == "xt");
    CHECK(js_true("items.length === 3 && items.nope === undefined"));
    CHECK(js_true("('1' in items) && (0 in items) && !(3 in items) && ('id' in items) && !('nope' in items)"));
    CHECK(js_true("Object.hasOwn(items, 2) && !Object.hasOwn(items, 3) && Object.hasOwn(items, 'id')"));

    // Descriptors: indices enumerable, names not; writable only where there is a setter.
    CHECK(js_str("JSON.stringify(Object.getOwnPropertyDescriptor(items, 1))") ==
          "{\"value\":\"b\",\"writable\":true,\"enumerable\":true,\"configurable\":true}");
    CHECK(js_str("JSON.stringify(Object.getOwnPropertyDescriptor(items, 'id'))") ==
          "{\"value\":\"x\",\"writable\":true,\"enumerable\":false,\"configurable\":true}");
    CHECK(js_str("JSON.stringify(Object.getOwnPropertyDescriptor(ro, 0))") ==
          "{\"value\":\"x\",\"writable\":false,\"enumerable\":true,\"configurable\":true}");

    // Keys: indices ascending, then names; enumerable ones for keys/for-in.
    CHECK(js_str("JSON.stringify(Object.keys(items))") == "[\"0\",\"1\",\"2\"]");
    CHECK(js_str("JSON.stringify(Object.getOwnPropertyNames(items))") == "[\"0\",\"1\",\"2\",\"id\",\"title\"]");
    CHECK(js_str("JSON.stringify(Object.keys(ro))") == "[\"0\",\"1\",\"id\",\"forEach\"]");
    CHECK(js_str("(() => { const seen = []; for (const k in items) seen.push(k); return seen.join(); })()") == "0,1,2,length");  // WebIDL attributes are enumerable
    CHECK(js_str("JSON.stringify(Reflect.ownKeys(items))") == "[\"0\",\"1\",\"2\",\"id\",\"title\"]");

    // Writing goes through the setters.
    r.Evaluate("items[1] = 'B'; items[3] = 'D'; items.id = 'y'; items.fresh = 'f';");
    CHECK(js_str("[...Array(4).keys()].map(i => items[i]).join('')") == "aBcD");
    CHECK(js_str("items.id + items.fresh") == "yf");
    CHECK(js_str("JSON.stringify(Object.getOwnPropertyNames(items).slice(-3))") == "[\"fresh\",\"id\",\"title\"]");
    CHECK(js_true("Reflect.defineProperty(items, 0, { value: 'Z' }) && items[0] === 'Z'"));
    CHECK(js_true("!Reflect.defineProperty(items, 0, { get() { return 1; } })"));
    CHECK(js_true("Reflect.set(items, 'id', 'w') && items.id === 'w'"));
    CHECK(js_true("delete items[3] && items[3] === undefined && items.length === 3"));
    CHECK(js_true("!Reflect.deleteProperty(items, 0) && items[0] === 'Z'"));
    CHECK(js_true("delete items.fresh && items.fresh === undefined && delete items.nope"));

    // No setters: sloppy writes fail quietly, strict ones throw, defineProperty reports false.
    r.Evaluate("ro[0] = 'changed'; ro[7] = 'new'; ro.id = 'changed';");
    CHECK(js_true("ro[0] === 'x' && ro[7] === undefined && ro.id === 'named:id'"));
    CHECK(js_true("(() => { 'use strict'; try { ro[0] = 1; } catch (e) { return e instanceof TypeError; } })()"));
    CHECK(js_true("(() => { 'use strict'; try { ro.id = 1; } catch (e) { return e instanceof TypeError; } })()"));
    CHECK(js_true("(() => { 'use strict'; try { delete ro[0]; } catch (e) { return e instanceof TypeError; } })()"));
    CHECK(js_true("!Reflect.defineProperty(ro, 0, { value: 1 }) && !Reflect.defineProperty(ro, 'id', { value: 1 })"));
    CHECK(js_true("delete ro[9] && !Reflect.deleteProperty(ro, 'id')"));
    CHECK(js_true("(() => { try { Object.defineProperty(ro, 0, { value: 1 }); } catch (e) { return e instanceof TypeError; } })()"));

    // A name a prototype already has is the prototype's, unless [LegacyOverrideBuiltIns].
    CHECK(js_true("typeof ro.forEach === 'string'"));  // ReadOnlyList.prototype has no forEach yet: the name shows
    r.Evaluate("ReadOnlyList.prototype.forEach = Array.prototype.forEach;");
    CHECK(js_true("typeof ro.forEach === 'function' && !Object.hasOwn(ro, 'forEach')"));
    CHECK(js_true("typeof ov.toString === 'string' && ov.toString === 'named:toString' && ov.plain === 'named:plain'"));
    CHECK(js_true("Object.hasOwn(ov, 'toString') && !Object.hasOwn(ov, 'valueOf')"));
    CHECK(js_true("typeof ov.valueOf === 'function'"));

    // Names that appear later are seen, whatever an earlier miss left in a cache.
    CHECK(js_str("(() => { const seen = []; for (let i = 0; i < 4; i++) { seen.push(String(items.late)); if (i === 1) addNamed(items, 'late', 'here'); } return seen.join(); })()") ==
          "undefined,undefined,here,here");

    // The array-like surface script gets from Array.prototype.
    r.Evaluate("Items.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];"
               "Items.prototype.forEach = Array.prototype.forEach; Items.prototype.map = Array.prototype.map;");
    CHECK(js_str("[...items].join('')") == "ZBc");
    CHECK(js_str("(() => { const out = []; items.forEach((v, i) => out.push(i + v)); return out.join(); })()") == "0Z,1B,2c");
    CHECK(js_str("Array.from(items).join('')") == "ZBc");
    CHECK(js_str("(() => { let n = 0; for (const v of items) n += v.length; return n; })()") == "3");
    CHECK(js_str("items.map(v => v + '!').join('')") == "Z!B!c!");
    CHECK(js_str("Array.prototype.slice.call(items, 1).join('')") == "Bc");
    CHECK(!HasException(realm->GetContext()));

    // Survives collection while script and host hold it.
    for (int i = 0; i < 3; i++) {
        isolate->CollectGarbage();
        CHECK(js_str("items[0] + items.id") == "Zw");
    }
}

// DOMString data keeps a lone surrogate; the USVString forms still replace it.
static void test_lone_surrogates() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Context& ctx = realm->GetContext();
    Embed::Realm& r = *realm;

    // Into script: lone high, lone low, a real pair, and a high directly before an ASCII.
    const char16_t units[] = {u'a', 0xD800, u'b', 0xDC00, u'c', 0xD83D, 0xDE00, 0xD801, u'd'};
    set_global(r, "s", FromUtf16(ctx, std::u16string_view(units, 9)));
    CHECK(eval_in(r, "s.length").as_number() == 9);
    CHECK(eval_in(r, "[...s].map(c => c.codePointAt(0).toString(16)).join()").to_string() == "61,d800,62,dc00,63,1f600,d801,64");
    CHECK(eval_in(r, "s.isWellFormed()").as_boolean() == false);

    // And back out, unchanged.
    std::u16string back = ToUtf16(ctx, global_of(r, "s"));
    CHECK(back == std::u16string(units, 9));
    std::string wtf8 = ToWtf8(ctx, global_of(r, "s"));
    CHECK(wtf8.find("\xED\xA0\x80") != std::string::npos);                    // lone U+D800, 3 bytes
    CHECK(wtf8.find("\xF0\x9F\x98\x80") != std::string::npos);                // the pair, 4 bytes
    Value again = FromWtf8(ctx, wtf8);
    set_global(r, "again", again);
    CHECK(eval_in(r, "again === s").as_boolean());

    // WTF-8 pieces that spell a pair are the pair; a script-made surrogate pair survives a round trip.
    Value pair = FromWtf8(ctx, "\xED\xA0\xBD\xED\xB8\x80");
    set_global(r, "pair", pair);
    CHECK(eval_in(r, "pair === '\\u{1F600}' && pair.length === 2").as_boolean());
    r.Evaluate("globalThis.made = 'x' + String.fromCharCode(0xDFFF) + String.fromCharCode(0xD800) + 'y';");
    std::u16string made = ToUtf16(ctx, global_of(r, "made"));
    CHECK(made == std::u16string({u'x', 0xDFFF, 0xD800, u'y'}));
    CHECK(eval_in(r, "made.charCodeAt(1) === 0xDFFF && made.charCodeAt(2) === 0xD800").as_boolean() &&
          FromUtf16(ctx, made).to_string() == global_of(r, "made").to_string());

    // The scalar-value forms are unchanged.
    CHECK(ToUsvUtf8(ctx, global_of(r, "s")).find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(FromUtf8(ctx, "a\xED\xA0\x80z").to_string() == "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDz");
    CHECK(ToUtf16(ctx, FromUtf8(ctx, "h\xC3\xA9llo \xF0\x9F\x98\x80")) == u"héllo \U0001F600");
    CHECK(!HasException(ctx));
}

// A block-bodied arrow inside a function reads that function's `arguments`, also when the
// function's body is read back lazily and the arrow's body is stepped over.
static void test_arrow_arguments() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    CHECK(eval_in(r, "(function f() { return (() => { return arguments.length; })(); })(1, 2)").as_number() == 2);
    CHECK(eval_in(r, "(function f() { return (() => { return (() => { return arguments[1]; })(); })(); })('a', 'b')").to_string() == "b");
    CHECK(eval_in(r, "(function f() { const g = () => { if (true) { return arguments[0]; } }; return g(); })(7)").as_number() == 7);
    CHECK(eval_in(r, "(function f() { return (() => { return typeof arguments; })(); })()").to_string() == "object");
}

// Redefining an accessor as a data property inside its own setter never runs that setter again,
// whether the object is small or large enough to be in dictionary mode (the global object is).
static void test_accessor_redefined_in_setter() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    r.Evaluate("globalThis.window = globalThis; for (let i = 0; i < 400; i++) globalThis['filler' + i] = i;");
    CHECK(eval_in(r, "(() => { let calls = 0;"
                     "Object.defineProperty(globalThis, 'log', { get() { return 'el'; }, enumerable: false, configurable: true,"
                     "  set(v) { calls++; Object.defineProperty(globalThis, 'log', { value: v }); } });"
                     "window.log = 'fn'; return window.log + calls; })()").to_string() == "fn1");
    CHECK(eval_in(r, "(() => { Object.defineProperty(globalThis, 'full', { get() { return 1; }, configurable: true,"
                     "  set(v) { Object.defineProperty(globalThis, 'full', { value: v, writable: true, enumerable: true, configurable: true }); } });"
                     "full = 2; full = 3; return JSON.stringify(Object.getOwnPropertyDescriptor(globalThis, 'full')); })()").to_string() ==
          "{\"value\":3,\"writable\":true,\"enumerable\":true,\"configurable\":true}");
    // The same result for a small object and a large one, including a conversion that fixes configurable.
    CHECK(eval_in(r, "(() => { const results = [];"
                     "for (const n of [5, 400]) { const o = {}; for (let i = 0; i < n; i++) o['k' + i] = i;"
                     "  Object.defineProperty(o, 'a', { get() { return 1; }, configurable: true });"
                     "  Object.defineProperty(o, 'a', { value: 9, configurable: false });"
                     "  Object.defineProperty(o, 'b', { get() { return 1; }, configurable: true });"
                     "  Object.defineProperty(o, 'b', { writable: true });"
                     "  results.push(JSON.stringify([Object.getOwnPropertyDescriptor(o, 'a'), Object.getOwnPropertyDescriptor(o, 'b')])); }"
                     "return results[0] === results[1]; })()").as_boolean());
}

static Object* g_elem_proto;
struct Elem : DOMObject {};

static Value ConstructElem(Context& ctx, Value, Args, Value newTarget) {
    if (IsUndefined(newTarget)) {
        ThrowTypeError(ctx, "Illegal constructor");
        return Undefined();
    }
    Object* proto = PrototypeFromNewTarget(ctx, newTarget);
    if (HasException(ctx)) return Undefined();
    Elem* e = Heap::Allocate<Elem>();
    e->initialize_prototype(proto ? proto : g_elem_proto);
    return FromObject(e);
}

// A Proxy's get trap that throws is what the caller sees, not an invariant violation about the
// value it never returned; and new.target.prototype is read once by a native constructor, whether
// it is reached directly, through a Proxy of it, or through a class that extends it.
static void test_proxy_get_and_construct_reads() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    ClassRef elem = DefineClass(r.GetContext(), "HTMLElement", ConstructElem, 0);
    g_elem_proto = elem.prototype;
    DefineGlobal(r.GetContext(), "HTMLElement", elem.constructor);

    for (const char* trap : {"get", "set", "deleteProperty"}) {
        std::string script = std::string("(() => { const target = class {}; Object.freeze(target);"
            "const p = new Proxy(target, { ") + trap + "() { throw new RangeError('boom'); } });"
            "try { " + (std::string(trap) == "get" ? "p.prototype;" : std::string(trap) == "set" ? "p.prototype = 1;" : "delete p.prototype;") +
            " return 'no throw'; } catch (e) { return e.constructor.name; } })()";
        CHECK(eval_in(r, script.c_str()).to_string() == "RangeError");
    }

    const char* counting = "var reads = 0; var proxy = new Proxy(function NT() {}, { get(t, k, rcv) { if (k === 'prototype') reads++; return Reflect.get(t, k, rcv); } });";
    auto reads_of = [&](const char* body) {
        r.Evaluate(std::string(counting) + "globalThis.__out = (() => { " + body + " return reads; })();");
        return global_of(r, "__out").as_number();
    };
    CHECK(reads_of("Reflect.construct(HTMLElement, [], proxy);") == 1);
    CHECK(reads_of("new (new Proxy(HTMLElement, { get(t, k, rcv) { if (k === 'prototype') reads++; return Reflect.get(t, k, rcv); } }))();") == 1);
    CHECK(reads_of("class Sub extends HTMLElement { constructor() { super(); } } Reflect.construct(Sub, [], proxy);") == 1);
    CHECK(reads_of("class Sub extends HTMLElement {} Reflect.construct(Sub, [], proxy);") == 1);
    CHECK(reads_of("Reflect.construct(Map, [], proxy);") == 1);
    // And the object still gets new.target's prototype.
    CHECK(eval_in(r, "(() => { function NT() {} NT.prototype = { marker: 1 };"
                     "class Sub extends HTMLElement { constructor() { super(); } }"
                     "class Plain extends HTMLElement {}"
                     "return [Reflect.construct(HTMLElement, [], NT), Reflect.construct(Sub, [], NT), Reflect.construct(Plain, [], NT)]"
                     ".every(o => Object.getPrototypeOf(o) === NT.prototype); })()").as_boolean());
}

// ---- The foundation of the embedding API ----------------------------------------

static int g_closures_alive = 0;
struct ClosureToken {
    ClosureToken() { g_closures_alive++; }
    ~ClosureToken() { g_closures_alive--; }
};

static Value make_counting_function(Context& ctx, std::shared_ptr<int> calls) {
    auto token = std::make_shared<ClosureToken>();
    return NewFunction(ctx, "counter", 1, [calls, token](Context& c, Value thisValue, Args args, Value newTarget) -> Value {
        ++*calls;
        if (!IsUndefined(newTarget)) return Undefined();
        return Value(ToNumber(c, args.empty() ? Undefined() : args[0]) + 1);
    });
}

// Kept out of line so that the Values it makes die with its frame, and the collections after it
// have only what the realm itself holds to go by.
[[gnu::noinline]] static void use_counting_closure(Embed::Isolate& isolate, Embed::Realm& r) {
    Context& ctx = r.GetContext();
    std::shared_ptr<int> calls = std::make_shared<int>(0);
    Persistent counter;
    {
        Value fn = make_counting_function(ctx, calls);
        counter = Persistent(ctx, fn);
    }
    CHECK(g_closures_alive == 1);
    Value arg = Value(41.0);
    CHECK(Call(ctx, counter.Get(), Undefined(), Args(&arg, 1)).as_number() == 42 && *calls == 1);
    set_global(r, "counter", counter.Get());
    CHECK(eval_in(r, "counter(1) + counter(2)").as_number() == 5 && *calls == 3);
    CHECK(eval_in(r, "(() => { try { new counter(); } catch (e) { return e instanceof TypeError; } })()").as_boolean());
    CHECK(eval_in(r, "counter.name + counter.length").to_string() == "counter1");
    for (int i = 0; i < 3; i++) { isolate.CollectGarbage(); CHECK(g_closures_alive == 1); }
    counter.Reset();
    r.Evaluate("counter = undefined;");
}

static void test_objects_and_conversions() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    Context& ctx = r.GetContext();
    auto js_bool = [&](const char* e) { return eval_in(r, e).as_boolean(); };

    // Conversions.
    CHECK(ToBoolean(Value(0.0)) == false && ToBoolean(FromUtf8(ctx, "x")) == true);
    r.Evaluate("globalThis.num = { valueOf() { return 41; } }; globalThis.thrower = { valueOf() { throw new RangeError('no'); } };"
               "globalThis.sym = Symbol('k');");
    CHECK(ToNumber(ctx, global_of(r, "num")) == 41);
    CHECK(std::isnan(ToNumber(ctx, global_of(r, "thrower"))) && HasException(ctx));
    ctx.clear_exception();
    CHECK(ToString(ctx, Value(12.5)).to_string() == "12.5");
    CHECK(IsSymbol(ToPropertyKey(ctx, global_of(r, "sym"))));
    CHECK(ToPropertyKey(ctx, Value(7.0)).to_string() == "7");
    CHECK(SameValue(Value(std::nan("")), Value(std::nan(""))) && !SameValue(Value(0.0), Value(-0.0)));
    CHECK(IsNumber(Value(1.0)) && IsString(FromUtf8(ctx, "s")) && IsBoolean(Value(true)) && IsNullish(Null()) && IsNullish(Undefined()));

    // Making values.
    Value object = NewObject(ctx);
    CHECK(IsObject(object) && eval_in(r, "Object.getPrototypeOf({}) === Object.prototype").as_boolean());
    Value items[3] = {Value(1.0), FromUtf8(ctx, "two"), Null()};
    Value array = NewArray(ctx, Args(items, 3));
    set_global(r, "made", array);
    CHECK(js_bool("Array.isArray(made) && made.length === 3 && made[1] === 'two' && made[2] === null"));
    set_global(r, "text", NewString(ctx, std::u16string_view(u"h\xe9llo \xD83D\xDE00")));
    CHECK(eval_in(r, "text.length").as_number() == 8);
    CHECK(NewString(ctx, "caf\xC3\xA9").to_string() == "caf\xC3\xA9");

    // IsArray, IsConstructor, InstanceOf.
    r.Evaluate("globalThis.arr = []; globalThis.pa = new Proxy([], {}); globalThis.rev = Proxy.revocable({}, {}); rev.revoke();"
               "globalThis.Klass = class Klass {}; globalThis.arrow = () => 1; globalThis.pk = new Proxy(Klass, {});"
               "globalThis.Even = { [Symbol.hasInstance](v) { return v % 2 === 0; } };");
    CHECK(IsArray(ctx, global_of(r, "arr")) && IsArray(ctx, global_of(r, "pa")) && !IsArray(ctx, global_of(r, "Klass")));
    CHECK(!HasException(ctx));
    CHECK(IsConstructor(global_of(r, "Klass")) && IsConstructor(global_of(r, "pk")) && !IsConstructor(global_of(r, "arrow")) &&
          !IsConstructor(Value(1.0)));
    CHECK(InstanceOf(ctx, object, global_of(r, "Object")) && !InstanceOf(ctx, Value(3.0), global_of(r, "Even")) &&
          InstanceOf(ctx, Value(4.0), global_of(r, "Even")));
    CHECK(!InstanceOf(ctx, object, Value(1.0)) && HasException(ctx));
    ctx.clear_exception();

    // Construct, with newTarget and through a Proxy's trap.
    Value instance = Construct(ctx, global_of(r, "Klass"));
    set_global(r, "instance", instance);
    CHECK(js_bool("instance instanceof Klass"));
    r.Evaluate("globalThis.Other = function Other() {}; Other.prototype = { marker: 1 };");
    Value made_with_target = Construct(ctx, global_of(r, "Klass"), {}, global_of(r, "Other"));
    set_global(r, "viaTarget", made_with_target);
    CHECK(js_bool("Object.getPrototypeOf(viaTarget) === Other.prototype"));
    r.Evaluate("globalThis.log = []; globalThis.trapped = new Proxy(Klass, { construct(t, args, nt) { log.push(args.length); return Reflect.construct(t, args, nt); } });");
    Value two[2] = {Value(1.0), Value(2.0)};
    Construct(ctx, global_of(r, "trapped"), Args(two, 2));
    CHECK(eval_in(r, "log.join()").to_string() == "2");
    Construct(ctx, global_of(r, "arrow"));
    CHECK(HasException(ctx));
    ctx.clear_exception();

    // The internal methods, with a Proxy's traps in the order the spec gives them.
    r.Evaluate("globalThis.trace = []; globalThis.target = { a: 1 }; Object.defineProperty(target, 'fixed', { value: 2, configurable: false });"
               "globalThis.prox = new Proxy(target, {"
               "  has(t, k) { trace.push('has:' + String(k)); return Reflect.has(t, k); },"
               "  deleteProperty(t, k) { trace.push('delete:' + String(k)); return Reflect.deleteProperty(t, k); },"
               "  getOwnPropertyDescriptor(t, k) { trace.push('gopd:' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },"
               "  defineProperty(t, k, d) { trace.push('define:' + String(k)); return Reflect.defineProperty(t, k, d); },"
               "  getPrototypeOf(t) { trace.push('getProto'); return Reflect.getPrototypeOf(t); },"
               "  setPrototypeOf(t, p) { trace.push('setProto'); return Reflect.setPrototypeOf(t, p); },"
               "  isExtensible(t) { trace.push('isExt'); return Reflect.isExtensible(t); },"
               "  preventExtensions(t) { trace.push('prevent'); return Reflect.preventExtensions(t); } });");
    Value prox = global_of(r, "prox");
    CHECK(HasProperty(ctx, prox, "a") && !HasProperty(ctx, prox, FromUtf8(ctx, "zz")));
    std::optional<Descriptor> d = GetOwnProperty(ctx, prox, "fixed");
    CHECK(d && d->has_value && d->value.as_number() == 2 && d->has_configurable && !d->configurable && d->has_writable && !d->writable);
    CHECK(!GetOwnProperty(ctx, prox, "nothing").has_value());
    Descriptor define;
    define.has_value = true; define.value = Value(9.0);
    define.has_writable = define.has_enumerable = define.has_configurable = true;
    define.writable = define.enumerable = define.configurable = true;
    CHECK(DefineProperty(ctx, prox, "b", define));
    CHECK(!DefineProperty(ctx, prox, "fixed", define));      // refused, no exception
    CHECK(!HasException(ctx));
    CHECK(DeleteProperty(ctx, prox, "b") && !DeleteProperty(ctx, prox, "fixed"));
    CHECK(GetPrototypeOf(ctx, prox).as_object() == global_of(r, "Object").as_object()->get_property("prototype").as_object());
    CHECK(SetPrototypeOf(ctx, prox, Null()) && IsNullish(GetPrototypeOf(ctx, prox)));
    CHECK(IsExtensible(ctx, prox) && PreventExtensions(ctx, prox) && !IsExtensible(ctx, prox));
    CHECK(eval_in(r, "trace.join()").to_string() ==
          "has:a,has:zz,gopd:fixed,gopd:nothing,define:b,define:fixed,delete:b,delete:fixed,getProto,setProto,getProto,isExt,prevent,isExt");
    Value elements = NewArray(ctx, Args(items, 3));
    CHECK(GetIndex(ctx, elements, 1).to_string() == "two");

    // A closure: carries its state, is not a constructor, and is destroyed with the function.
    use_counting_closure(*isolate, r);
    scrub_stack();
    isolate->CollectGarbage();
    isolate->CollectGarbage();
    CHECK(g_closures_alive == 0);

    // Errors of the realm, and a host class of the same shape.
    r.Evaluate("globalThis.DOMException = class DOMException extends Error { constructor(m, n) { super(m); this.name = n; } };");
    Value range = NewError(ctx, "RangeError", "too far");
    set_global(r, "range", range);
    CHECK(js_bool("range instanceof RangeError && range.message === 'too far'"));
    ThrowError(ctx, "SyntaxError", "bad");
    CHECK(HasException(ctx) && ctx.get_exception().is_object());
    ctx.clear_exception();
    ThrowRangeError(ctx, "r");
    CHECK(HasException(ctx));
    ctx.clear_exception();
    ThrowSyntaxError(ctx, "s");
    ThrowReferenceError(ctx, "x");
    ctx.clear_exception();
    ThrowError(ctx, "DOMException", "gone");
    set_global(r, "thrown", ctx.get_exception());
    ctx.clear_exception();
    CHECK(js_bool("thrown instanceof DOMException && thrown instanceof Error && thrown.message === 'gone'"));
    NewError(ctx, "NotAClass", "x");
    CHECK(HasException(ctx));
    ctx.clear_exception();
    Throw(ctx, FromUtf8(ctx, "just a string"));
    CHECK(HasException(ctx) && ctx.get_exception().to_string() == "just a string");
    ctx.clear_exception();
}

static int g_released = 0;
static void release_buffer(void* data, void* user) {
    g_released++;
    std::free(data);
    (void)user;
}

static Value make_external_buffer(Context& ctx, size_t size) {
    uint8_t* memory = static_cast<uint8_t*>(std::malloc(size));
    for (size_t i = 0; i < size; i++) memory[i] = static_cast<uint8_t>(i);
    return NewArrayBuffer(ctx, memory, size, release_buffer, nullptr);
}

[[gnu::noinline]] static void make_global_external(Embed::Realm& realm, const char* name, size_t size) {
    set_global(realm, name, make_external_buffer(realm.GetContext(), size));
}

[[gnu::noinline]] static void use_buffers(Embed::Realm& r) {
    Context& ctx = r.GetContext();
    {
        Value buffer = make_external_buffer(ctx, 16);
        set_global(r, "ext", buffer);
        CHECK(eval_in(r, "ext instanceof ArrayBuffer && ext.byteLength === 16 && new Uint8Array(ext)[5] === 5").as_boolean());
        // The same bytes: script writes show through the span and the host's show in script.
        r.Evaluate("new Uint8Array(ext)[0] = 99;");
        auto bytes = MutableBytesOf(buffer);
        CHECK(bytes && bytes->size() == 16 && (*bytes)[0] == 99);
        (*bytes)[1] = 77;
        CHECK(eval_in(r, "new Uint8Array(ext)[1]").as_number() == 77);
    }
    Value zeroed = NewArrayBuffer(ctx, 8);
    set_global(r, "zeroed", zeroed);
    CHECK(eval_in(r, "zeroed.byteLength === 8 && new Uint8Array(zeroed).every(b => b === 0)").as_boolean());
    Value window = NewUint8Array(ctx, global_of(r, "ext"), 4, 3);
    set_global(r, "win", window);
    CHECK(eval_in(r, "win.length === 3 && win.byteOffset === 4 && win[0] === 4 && win.buffer === ext").as_boolean());

    // Shared, detach, transfer.
    Value shared = NewSharedArrayBuffer(ctx, 4);
    CHECK(IsSharedArrayBuffer(shared) && !IsSharedArrayBuffer(zeroed) && !HasException(ctx));
    CHECK(!DetachArrayBuffer(ctx, shared) && HasException(ctx));
    ctx.clear_exception();
    CHECK(!IsDetached(zeroed));
    Value moved = TransferArrayBuffer(ctx, zeroed);
    CHECK(IsDetached(zeroed) && !IsDetached(moved) && BytesOf(moved) && BytesOf(moved)->size() == 8 && !BytesOf(zeroed));
    set_global(r, "tail", window);
    CHECK(DetachArrayBuffer(ctx, global_of(r, "ext")) && IsDetached(global_of(r, "ext")) && IsDetached(window));
    CHECK(eval_in(r, "win.length === 0").as_boolean());

}

static void test_buffers_zero_copy() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    Context& ctx = r.GetContext();

    use_buffers(r);

    // The host's memory is given back once nothing holds the buffer.
    r.Evaluate("ext = win = tail = undefined;");
    scrub_stack();
    for (int i = 0; i < 3; i++) isolate->CollectGarbage();
    CHECK(g_released == 1);
    make_global_external(r, "kept", 4);
    isolate->CollectGarbage();
    CHECK(g_released == 1);
    r.Evaluate("kept = undefined;");
    scrub_stack();
    for (int i = 0; i < 3; i++) isolate->CollectGarbage();
    CHECK(g_released == 2);
}

static int g_default_marker;
static Value ConstructMarked(Context& ctx, Value, Args, Value newTarget) {
    if (IsUndefined(newTarget)) {
        ThrowTypeError(ctx, "new");
        return Undefined();
    }
    Object* proto = PrototypeFromNewTarget(ctx, newTarget, &g_default_marker);
    if (HasException(ctx)) return Undefined();
    Elem* e = Heap::Allocate<Elem>();
    e->initialize_prototype(proto);
    return FromObject(e);
}

static void test_realm_helpers() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    std::unique_ptr<Embed::Realm> b = isolate->CreateRealm();
    for (Embed::Realm* realm : {a.get(), b.get()}) {
        Context& ctx = realm->GetContext();
        ClassRef k = DefineClass(ctx, "Marked", ConstructMarked, 0);
        SetRealmData(ctx, &g_default_marker, k.prototype);
        DefineGlobal(ctx, "Marked", k.constructor);
    }

    // Which realm a context and a function belong to.
    CHECK(Embed::Realm::FromContext(a->GetContext()) == a.get() && Embed::Realm::FromContext(b->GetContext()) == b.get());
    b->Evaluate("globalThis.bFn = function () {}; globalThis.bArrow = () => 1; globalThis.bBound = bFn.bind(null);"
                "globalThis.bProxy = new Proxy(bFn, {}); globalThis.bRevoked = Proxy.revocable(function () {}, {});");
    set_global(*a, "bFn", global_of(*b, "bFn"));
    set_global(*a, "bArrow", global_of(*b, "bArrow"));
    set_global(*a, "bBound", global_of(*b, "bBound"));
    set_global(*a, "bProxy", global_of(*b, "bProxy"));
    a->Evaluate("globalThis.aFn = function () {};");
    Context& ca = a->GetContext();
    CHECK(GetFunctionRealm(ca, global_of(*a, "aFn")) == a.get());
    CHECK(GetFunctionRealm(ca, global_of(*a, "bFn")) == b.get());
    CHECK(GetFunctionRealm(ca, global_of(*a, "bArrow")) == b.get());
    CHECK(GetFunctionRealm(ca, global_of(*a, "bBound")) == b.get());
    CHECK(GetFunctionRealm(ca, global_of(*a, "bProxy")) == b.get());
    CHECK(GetFunctionRealm(ca, global_of(*a, "Marked")) == a.get());

    // Cross-realm subclassing: newTarget without a prototype object gets the default of ITS realm.
    a->Evaluate("globalThis.NT = function () {}; NT.prototype = null;");
    b->Evaluate("globalThis.NT = function () {}; NT.prototype = null;");
    set_global(*a, "bNT", global_of(*b, "NT"));
    set_global(*a, "BMarked", global_of(*b, "Marked"));
    CHECK(eval_in(*a, "(() => { const o = Reflect.construct(Marked, [], bNT); return Object.getPrototypeOf(o) === BMarked.prototype; })()").as_boolean());
    CHECK(eval_in(*a, "(() => { const o = Reflect.construct(Marked, [], NT); return Object.getPrototypeOf(o) === Marked.prototype; })()").as_boolean());
    CHECK(eval_in(*a, "(() => { const o = Reflect.construct(Marked, [], bProxy); return Object.getPrototypeOf(o) !== Marked.prototype; })()").as_boolean());

    // Run: host code inside a realm. What it makes belongs to that realm.
    Value made_in_b;
    b->Run([&] {
        Context& cb = b->GetContext();
        made_in_b = NewArray(cb, {});
        set_global(*b, "viaRun", made_in_b);
        Value fn = global_of(*b, "bFn");
        CHECK(Embed::Realm::FromContext(cb) == b.get());
    });
    set_global(*a, "viaRun", made_in_b);
    CHECK(eval_in(*a, "Array.isArray(viaRun) && !(viaRun instanceof Array)").as_boolean());
    CHECK(eval_in(*b, "viaRun instanceof Array").as_boolean());
}

// ---- Reporting what nobody caught ------------------------------------------------

static Embed::Isolate* g_report_isolate;

static void test_error_reporting() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    g_report_isolate = isolate.get();
    std::unique_ptr<Embed::Realm> a = isolate->CreateRealm();
    Embed::Realm& r = *a;
    Context& ctx = r.GetContext();

    // What Evaluate says about a script that throws, and one that does not parse.
    Embed::EvaluateResult thrown = r.Evaluate("function inner() { throw new RangeError('deep'); }\nfunction outer() { inner(); }\nouter();", "https://example.test/app.js");
    CHECK(!thrown.ok && thrown.exception.is_object());
    CHECK(thrown.filename == "https://example.test/app.js" && thrown.line > 0);
    CHECK(thrown.stack.size() >= 2 && thrown.stack[0].function == "inner" && thrown.stack[1].function == "outer");
    CHECK(thrown.stack[0].filename == "https://example.test/app.js" && thrown.stack[0].line > 0);
    Embed::ErrorInfo info = InspectError(ctx, thrown.exception);
    CHECK(info.is_error && info.name == "RangeError" && info.message == "deep" && !info.has_cause);
    CHECK(info.stack.rfind("RangeError: deep", 0) == 0);

    Embed::EvaluateResult syntax = r.Evaluate("let ok = 1;\nlet = = 2;", "bad.js");
    CHECK(!syntax.ok && syntax.line == 2 && syntax.column > 0 && syntax.filename == "bad.js" && syntax.stack.empty());
    CHECK(syntax.exception.is_object() && InspectError(ctx, syntax.exception).name == "SyntaxError");
    CHECK(eval_in(r, "1 + 1").as_number() == 2);

    // Causes, and values that are not errors.
    r.Evaluate("globalThis.chained = new Error('outer', { cause: new TypeError('why') });");
    Embed::ErrorInfo chained = InspectError(ctx, global_of(r, "chained"));
    CHECK(chained.has_cause && InspectError(ctx, chained.cause).name == "TypeError");
    Embed::ErrorInfo plain = InspectError(ctx, FromUtf8(ctx, "just text"));
    CHECK(!plain.is_error && plain.message == "just text" && plain.frames.empty());
    Embed::ErrorInfo number = InspectError(ctx, Value(42.0));
    CHECK(!number.is_error && number.message == "42");

    // Exceptions with no script left to catch them.
    struct Seen { Embed::Realm* realm; std::string origin, name, message; bool is_error; size_t frames; };
    std::vector<Seen> seen;
    isolate->SetUncaughtExceptionHandler([&](const Embed::UncaughtException& e) {
        seen.push_back({e.realm, e.origin, e.info.name, e.info.message, e.info.is_error, e.info.frames.size()});
    });
    r.Evaluate("setTimeout(function failing() { throw new Error('from a timer'); }, 0);"
               "queueMicrotask(() => { throw 'a string'; });");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    isolate->RunDueTimers();
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].origin == "queueMicrotask" && !seen[0].is_error && seen[0].message == "a string" && seen[0].realm == &r);
        CHECK(seen[1].origin == "timer" && seen[1].is_error && seen[1].name == "Error" && seen[1].message == "from a timer" &&
              seen[1].frames >= 1 && seen[1].realm == &r);
    }
    isolate->SetUncaughtExceptionHandler(nullptr);

    // Promise rejections: unhandled when rejected, handled when a handler turns up.
    struct Rejection { Embed::Realm* realm; Embed::RejectionEvent event; std::string reason; };
    std::vector<Rejection> rejections;
    isolate->SetPromiseRejectionHandler([&](Embed::Realm* realm, const Value& promise, const Value& reason, Embed::RejectionEvent event) {
        CHECK(promise.is_object());
        rejections.push_back({realm, event, InspectError(realm->GetContext(), reason).message});
    });
    r.Evaluate("globalThis.late = Promise.reject(new Error('nobody'));");
    CHECK(rejections.size() == 1 && rejections[0].event == Embed::RejectionEvent::Unhandled && rejections[0].reason == "nobody" &&
          rejections[0].realm == &r);
    r.Evaluate("late.catch(() => {});");
    CHECK(rejections.size() == 2 && rejections[1].event == Embed::RejectionEvent::Handled);
    // Attached in the same turn, a handler still gives the host both events: it can cancel what it queued.
    r.Evaluate("Promise.reject(new Error('handled at once')).catch(() => {});");
    CHECK(rejections.size() == 4 && rejections[2].event == Embed::RejectionEvent::Unhandled &&
          rejections[3].event == Embed::RejectionEvent::Handled);
    r.Evaluate("(async () => { throw new Error('async'); })();");
    CHECK(rejections.size() == 5 && rejections[4].reason == "async" && rejections[4].event == Embed::RejectionEvent::Unhandled);
    isolate->SetPromiseRejectionHandler(nullptr);

    // Whether any script is running, and a checkpoint from inside one.
    DefineGlobalFunction(ctx, "stackEmpty", [](Context&, Value, Args, Value) { return Value(g_report_isolate->JsStackEmpty()); }, 0);
    DefineGlobalFunction(ctx, "checkpoint", [](Context&, Value, Args, Value) {
        g_report_isolate->PerformMicrotaskCheckpoint();
        return Undefined();
    }, 0);
    CHECK(isolate->JsStackEmpty());
    CHECK(!eval_in(r, "stackEmpty()").as_boolean());
    r.Evaluate("globalThis.order = [];"
               "Promise.resolve().then(() => order.push('job'));"
               "checkpoint(); order.push('after');"
               "Promise.resolve().then(() => { Promise.resolve().then(() => order.push('inner'));"
               "  checkpoint(); order.push('outer-end'); });");
    CHECK(eval_in(r, "order.join()").to_string() == "job,after,outer-end,inner");
    CHECK(isolate->JsStackEmpty());
    g_report_isolate = nullptr;
}

// ---- Finalization, weak handles and traced members ------------------------------

static std::set<void*> g_registry;           // a host's own bookkeeping of live nodes
static int g_finalized, g_peer_intact;

struct Node : DOMObject {
    static constexpr uint32_t kAlive = 0xA11CE;
    uint32_t sentinel = kAlive;
    int id;
    Node* peer = nullptr;                     // a raw pointer to another node: not traced, not owned
    explicit Node(int id) : id(id) { g_registry.insert(this); }
    // Everything is still intact here: the peer may be read, dead or not.
    void Finalize() {
        g_finalized++;
        if (peer && peer->sentinel == kAlive && peer->id != 0) g_peer_intact++;
        g_registry.erase(this);
    }
    // The destructor may free what the object owns and nothing else.
    ~Node() { sentinel = 0xDEAD; }
};

[[gnu::noinline]] static void make_node_pair(Embed::Realm& r) {
    Node* a = Heap::Allocate<Node>(1);
    Node* b = Heap::Allocate<Node>(2);
    a->peer = b;
    b->peer = a;
    set_global(r, "nodeA", FromObject(a));
    set_global(r, "nodeB", FromObject(b));
}

struct TracedHolder : DOMObject {
    TracedValue one;
    TracedList list;
};

[[gnu::noinline]] static TracedHolder* make_traced_holder(Embed::Realm& r) {
    TracedHolder* holder = Heap::Allocate<TracedHolder>();
    set_global(r, "holder", FromObject(holder));
    return holder;
}

[[gnu::noinline]] static void store_young_values(Embed::Realm& r, TracedHolder* holder) {
    r.Evaluate("globalThis.young = { tag: 'one' }; globalThis.young2 = { tag: 'two' };");
    holder->one.Set(global_of(r, "young"));
    holder->list.push_back(global_of(r, "young2"));
    r.Evaluate("young = young2 = undefined;");
}

[[gnu::noinline]] static std::string read_tag(Context& ctx, const Value& object) {
    return Get(ctx, object, "tag").to_string();
}

static void test_finalization_and_weak() {
    g_registry.clear();
    g_finalized = g_peer_intact = 0;
    {
        std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
        std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
        Embed::Realm& r = *realm;
        Context& ctx = r.GetContext();

        // Finalize runs once, before any destructor, with every cell of the collection still intact.
        make_node_pair(r);
        WeakHandle<Node> weak_a(DOMObject::Cast<Node>(global_of(r, "nodeA")));
        WeakHandle<Node> weak_copy = weak_a;
        r.Evaluate("globalThis.scriptObject = { tag: 'js' };");
        WeakHandle<Object> weak_script(global_of(r, "scriptObject").as_object());
        CHECK(weak_a.Get() && weak_copy.Get() == weak_a.Get() && weak_script.Get() && g_registry.size() == 2);
        isolate->CollectGarbage();
        CHECK(g_finalized == 0 && weak_a.Get() && weak_script.Get());

        r.Evaluate("nodeA = nodeB = scriptObject = undefined;");
        scrub_stack();
        isolate->CollectGarbage();
        isolate->CollectGarbage();
        CHECK(g_finalized == 2 && g_peer_intact == 2 && g_registry.empty());
        CHECK(weak_a.Get() == nullptr && weak_copy.Get() == nullptr && weak_copy.IsEmpty() && weak_script.Get() == nullptr);

        // Traced members: traced and written through the barrier by themselves.
        TracedHolder* holder = make_traced_holder(r);
        isolate->CollectGarbage();               // the holder is old now
        store_young_values(r, holder);
        scrub_stack();
        Collector::collect_minor();              // an old object's new edges must not be missed
        CHECK(read_tag(ctx, holder->one.Get()) == "one" && read_tag(ctx, holder->list[0]) == "two");
        isolate->CollectGarbage();
        isolate->CollectGarbage();
        CHECK(read_tag(ctx, holder->one.Get()) == "one" && read_tag(ctx, holder->list[0]) == "two" && holder->list.size() == 1);
        holder->list.clear();
        holder->one.Clear();
        CHECK(holder->list.empty() && holder->one.Get().is_undefined());

        // Teardown: everything still alive is finalized before the heap goes.
        r.Evaluate("globalThis.keep = undefined;");
        make_node_pair(r);
        CHECK(g_registry.size() == 2);
    }
    CHECK(g_registry.empty() && g_finalized == 4);
}

// ---- Inspecting values -------------------------------------------------------------

static void test_inspect() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    Context& ctx = r.GetContext();
    using K = Embed::ObjectKind;

    r.Evaluate(
        "globalThis.sideEffects = 0;"
        "class Point { constructor() { this.x = 1; } get norm() { sideEffects++; return 1; } }"
        "const sym = Symbol('tag');"
        "globalThis.samples = {"
        "  plain: { a: 1, get lazy() { sideEffects++; return 2; }, set lazy(v) {}, [sym]: 'symval' },"
        "  point: new Point(), arr: [1, 2, 3], fn: function named(a, b) { return a + b; }, arrow: (x) => x,"
        "  klass: Point, asyncFn: async function af() {}, gen: function* g() {}, bound: function b0() {}.bind(null),"
        "  native: Math.max, err: new RangeError('bad'), date: new Date(86400000), badDate: new Date(NaN), re: /a+b/gi,"
        "  map: new Map([[1, 'one'], ['k', { v: 2 }]]), set: new Set([1, 'two']), weakMap: new WeakMap(), weakSet: new WeakSet(),"
        "  weakRef: new WeakRef({}), pending: new Promise(() => {}), ok: Promise.resolve(42), no: Promise.reject(new Error('r')),"
        "  proxy: new Proxy({ t: 1 }, { get() { sideEffects++; return 5; }, ownKeys() { sideEffects++; return []; } }),"
        "  u8: new Uint8Array(4), f64: new Float64Array(2), buf: new ArrayBuffer(8), sab: new SharedArrayBuffer(2), dv: new DataView(new ArrayBuffer(6), 2),"
        "  boxedNum: new Number(7), boxedStr: new String('s'), boxedBool: new Boolean(false), boxedSym: Object(Symbol('b')), boxedBig: Object(5n),"
        "  generator: (function* () { yield 1; })(), iterator: [1][Symbol.iterator](), args: (function () { return arguments; })(1, 2),"
        "  frozen: Object.freeze({}), nullProto: Object.create(null) };"
        "samples.no.catch(() => {});"
        "const dead = Proxy.revocable({}, {}); dead.revoke(); samples.revoked = dead.proxy;");
    Value samples = global_of(r, "samples");
    auto info_of = [&](const char* name) { return Inspect(ctx, Get(ctx, samples, name)); };

    CHECK(!Inspect(ctx, Value(1.0)).is_object && !Inspect(ctx, FromUtf8(ctx, "s")).is_object);
    Embed::ObjectInfo plain = info_of("plain");
    CHECK(plain.is_object && plain.kind == K::Plain && plain.class_name == "Object" && plain.extensible && plain.id != 0);
    CHECK(info_of("point").class_name == "Point" && info_of("point").kind == K::Plain);
    CHECK(info_of("arr").kind == K::Array && info_of("arr").size == 3 && info_of("arr").class_name == "Array");
    CHECK(info_of("frozen").extensible == false);
    CHECK(info_of("nullProto").class_name.empty() && info_of("nullProto").prototype.is_null());
    CHECK(info_of("args").kind == K::Arguments);

    Embed::ObjectInfo fn = info_of("fn");
    CHECK(fn.kind == K::Function && fn.function && fn.function->name == "named" && fn.function->length == 2 &&
          !fn.function->is_class && !fn.function->is_arrow && fn.function->is_constructor && !fn.function->is_native &&
          fn.function->source.find("a + b") != std::string::npos);
    CHECK(info_of("arrow").function->is_arrow && !info_of("arrow").function->is_constructor);
    CHECK(info_of("klass").function->is_class && info_of("klass").function->name == "Point");
    CHECK(info_of("asyncFn").function->is_async && !info_of("asyncFn").function->is_generator);
    CHECK(info_of("gen").function->is_generator && !info_of("gen").function->is_async);
    CHECK(info_of("bound").function->is_bound);
    CHECK(info_of("native").function->is_native && info_of("native").function->name == "max" &&
          info_of("native").function->source.find("[native code]") != std::string::npos);

    CHECK(info_of("err").kind == K::Error && info_of("err").class_name == "RangeError");
    CHECK(info_of("date").kind == K::Date && info_of("date").date_value == 86400000.0 && std::isnan(info_of("badDate").date_value));
    Embed::ObjectInfo re = info_of("re");
    CHECK(re.kind == K::RegExp && re.regexp_source == "a+b" && re.regexp_flags == "gi");
    Embed::ObjectInfo map = info_of("map");
    CHECK(map.kind == K::Map && map.size == 2 && info_of("set").kind == K::Set && info_of("set").size == 2);
    CHECK(info_of("weakMap").kind == K::WeakMap && info_of("weakSet").kind == K::WeakSet && info_of("weakRef").kind == K::WeakRef);

    ValueList entries = MapEntries(Get(ctx, samples, "map"));
    CHECK(entries.size() == 4 && entries[0].as_number() == 1 && entries[1].to_string() == "one" && entries[2].to_string() == "k" &&
          Get(ctx, entries[3], "v").as_number() == 2);
    ValueList members = SetValues(Get(ctx, samples, "set"));
    CHECK(members.size() == 2 && members[0].as_number() == 1 && members[1].to_string() == "two" && MapEntries(samples).size() == 0);

    Embed::ObjectInfo pending = info_of("pending"), ok = info_of("ok"), no = info_of("no");
    CHECK(pending.kind == K::Promise && pending.promise_state == Embed::PromiseState::Pending && pending.promise_result.is_undefined());
    CHECK(ok.promise_state == Embed::PromiseState::Fulfilled && ok.promise_result.as_number() == 42);
    CHECK(no.promise_state == Embed::PromiseState::Rejected && InspectError(ctx, no.promise_result).message == "r");

    Embed::ObjectInfo proxy = info_of("proxy");
    CHECK(proxy.kind == K::Proxy && !proxy.proxy_revoked && Get(ctx, proxy.proxy_target, "t").as_number() == 1 && proxy.proxy_handler.is_object());
    Embed::ObjectInfo revoked = info_of("revoked");
    CHECK(revoked.kind == K::Proxy && revoked.proxy_revoked && revoked.proxy_target.is_null());
    CHECK(InspectProperties(ctx, Get(ctx, samples, "proxy")).size() == 0);

    CHECK(info_of("u8").kind == K::TypedArray && info_of("u8").element_type == "Uint8Array" && info_of("u8").size == 4 && info_of("u8").byte_length == 4);
    CHECK(info_of("f64").element_type == "Float64Array" && info_of("f64").byte_length == 16);
    CHECK(info_of("buf").kind == K::ArrayBuffer && info_of("buf").byte_length == 8 && !info_of("buf").detached);
    CHECK(info_of("sab").kind == K::SharedArrayBuffer && info_of("dv").kind == K::DataView && info_of("dv").byte_length == 4);
    CHECK(info_of("boxedNum").kind == K::BoxedNumber && info_of("boxedNum").primitive_value.as_number() == 7);
    CHECK(info_of("boxedStr").kind == K::BoxedString && info_of("boxedStr").primitive_value.to_string() == "s");
    CHECK(info_of("boxedBool").kind == K::BoxedBoolean && !info_of("boxedBool").primitive_value.as_boolean());
    CHECK(info_of("boxedSym").kind == K::BoxedSymbol && info_of("boxedBig").kind == K::BoxedBigInt);
    CHECK(info_of("generator").kind == K::Generator && info_of("iterator").kind == K::Iterator);

    // Properties come back as descriptors; nothing was called to make them.
    Embed::PropertyList props = InspectProperties(ctx, Get(ctx, samples, "plain"));
    CHECK(props.size() == 3);
    if (props.size() == 3) {
        CHECK(props[0].key.to_string() == "a" && props[0].has_value && props[0].value.as_number() == 1 && props[0].writable && props[0].enumerable);
        CHECK(props[1].key.to_string() == "lazy" && !props[1].has_value && props[1].has_getter && props[1].has_setter && props[1].getter.is_function());
        CHECK(IsSymbol(props[2].key) && props[2].value.to_string() == "symval");
    }
    CHECK(eval_in(r, "sideEffects").as_number() == 0);   // no getter, no trap ran
    Embed::PropertyList point_props = InspectProperties(ctx, Get(ctx, samples, "point"));
    CHECK(point_props.size() == 1 && point_props[0].key.to_string() == "x");
    Embed::PropertyList proto_props = InspectProperties(ctx, Get(ctx, Get(ctx, samples, "klass"), "prototype"));
    bool norm_is_accessor = false;
    for (const Embed::PropertyInfo& p : proto_props) if (p.key.to_string() == "norm") norm_is_accessor = p.has_getter && !p.has_value && !p.enumerable;
    CHECK(norm_is_accessor && eval_in(r, "sideEffects").as_number() == 0);

    // Identity: stable for an object, different between objects, which is what finds a cycle.
    r.Evaluate("globalThis.loop = { name: 'loop' }; loop.self = loop; loop.child = { parent: loop };");
    Value loop = global_of(r, "loop");
    std::set<uint64_t> on_path;
    std::function<bool(const Value&, int)> has_cycle = [&](const Value& v, int depth) {
        Embed::ObjectInfo i = Inspect(ctx, v);
        if (!i.is_object || depth > 8) return false;
        if (!on_path.insert(i.id).second) return true;
        for (const Embed::PropertyInfo& p : InspectProperties(ctx, v)) {
            if (p.has_value && has_cycle(p.value, depth + 1)) { on_path.erase(i.id); return true; }
        }
        on_path.erase(i.id);
        return false;
    };
    CHECK(has_cycle(loop, 0) && Inspect(ctx, loop).id == Inspect(ctx, global_of(r, "loop")).id && Inspect(ctx, loop).id != info_of("plain").id);
    CHECK(!has_cycle(Get(ctx, samples, "arr"), 0));

    // Detached buffers say so, through the view too.
    r.Evaluate("globalThis.moving = new ArrayBuffer(4); globalThis.movingView = new Uint8Array(moving); moving.transfer();");
    CHECK(Inspect(ctx, global_of(r, "moving")).detached && Inspect(ctx, global_of(r, "movingView")).detached &&
          Inspect(ctx, global_of(r, "movingView")).size == 0);

    // A realm without Quanta's console.
    std::unique_ptr<Embed::Realm> quiet = isolate->CreateRealm(Embed::Isolate::RealmOptions{false});
    CHECK(eval_in(*quiet, "typeof console").to_string() == "undefined");
    CHECK(eval_in(r, "typeof console").to_string() == "object");
    quiet->Evaluate("globalThis.console = { log() { return 'mine'; } };");
    CHECK(eval_in(*quiet, "console.log()").to_string() == "mine");
}

// With tracking on a frame is placed at the call it is making; off, at where its function is declared.
static void test_source_positions() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    const char* script =
        "function inner() {\n"
        "  var x = 1;\n"
        "  throw new RangeError('deep');\n"
        "}\n"
        "function middle() {\n"
        "  var y = 2;\n"
        "  inner();\n"
        "}\n"
        "var obj = { run() {\n"
        "  middle(); } };\n"
        "obj.run();\n";

    Embed::EvaluateResult off = r.Evaluate(script, "app.js");
    CHECK(!off.ok && off.stack.size() >= 3 && off.stack[0].function == "inner" && off.stack[0].line == 1);

    isolate->SetSourcePositionTracking(true);
    Embed::EvaluateResult on = r.Evaluate(script, "app.js");
    CHECK(!on.ok && on.stack.size() >= 4);
    if (on.stack.size() >= 4) {
        CHECK(on.stack[0].function == "inner" && on.stack[0].line == 3 && on.stack[0].column == 3 && on.stack[0].filename == "app.js");
        CHECK(on.stack[1].function == "middle" && on.stack[1].line == 7 && on.stack[1].column == 3);
        CHECK(on.stack[2].function == "run" && on.stack[2].line == 10);
        CHECK(on.stack[3].function == "<anonymous>" && on.stack[3].line == 11 && on.stack[3].filename == "app.js");
    }
    CHECK(on.line == 3 && on.column == 3);

    // Through a generator and an async function: no crash, and the frames outside are still placed.
    Embed::EvaluateResult through_fibers = r.Evaluate(
        "function* gen() { yield thrower(); }\n"
        "function thrower() { return new Error('from gen').stack; }\n"
        "globalThis.fromGenerator = gen().next().value;\n"
        "(async function () { await null; globalThis.fromAsync = thrower(); })();\n",
        "fibers.js");
    CHECK(through_fibers.ok);
    CHECK(eval_in(r, "fromGenerator.includes('at <anonymous> (fibers.js:3:')").as_boolean());
    CHECK(eval_in(r, "typeof fromAsync").to_string() == "string");

    // Switching it off again takes effect for the calls made after.
    isolate->SetSourcePositionTracking(false);
    Embed::EvaluateResult again = r.Evaluate(script, "app.js");
    CHECK(!again.ok && again.stack[0].line == 1);
    CHECK(eval_in(r, "1 + 1").as_number() == 2);
}

// ---- ES modules through the host -----------------------------------------------------

namespace {
struct ModuleWorld {
    std::map<std::string, Embed::ModuleSource> files;
    std::vector<std::string> fetched;
    std::vector<std::pair<std::string, std::function<void(Embed::ModuleSource)>>> parked;   // answered later
    bool park = false;
    bool deny_dynamic = false;
};

static std::string join_url(const std::string& referrer, const std::string& specifier) {
    if (specifier.rfind("http://", 0) == 0) return specifier;
    std::string base = referrer.substr(0, referrer.rfind('/') + 1);
    if (specifier.rfind("./", 0) == 0) return base + specifier.substr(2);
    return "http://host/" + specifier;
}

static void install_module_world(Embed::Isolate& isolate, ModuleWorld& world) {
    Embed::ModuleHooks hooks;
    hooks.resolve = [](Embed::Realm*, const std::string& specifier, const std::string& referrer, std::string& out, std::string& error) {
        if (specifier == "forbidden") { error = "no such package"; return false; }
        out = join_url(referrer, specifier);
        return true;
    };
    hooks.fetch = [&world](Embed::Realm*, const std::string& url, const std::string&, std::function<void(Embed::ModuleSource)> done) {
        world.fetched.push_back(url);
        auto it = world.files.find(url);
        Embed::ModuleSource answer = it == world.files.end() ? Embed::ModuleSource::Failure("404 " + url) : it->second;
        if (world.park) world.parked.emplace_back(url, [done, answer](Embed::ModuleSource) mutable { done(answer); });
        else done(answer);
    };
    hooks.initImportMeta = [](Embed::Realm* realm, const Value& meta, const std::string& url) {
        Context& ctx = realm->GetContext();
        Embed::Set(ctx, meta, "url", Embed::FromUtf8(ctx, url));
    };
    hooks.dynamicImport = [&world](Embed::Realm*, const std::string& specifier, const std::string&, const std::string&) -> std::optional<std::string> {
        if (world.deny_dynamic) return "blocked: " + specifier;
        return std::nullopt;
    };
    isolate.SetModuleHooks(std::move(hooks));
}

// A promise's state and result, after the jobs have run.
static Embed::ObjectInfo settled(Embed::Isolate& isolate, Embed::Realm& r, const Value& promise) {
    isolate.PerformMicrotaskCheckpoint();
    return Embed::Inspect(r.GetContext(), promise);
}
}

static void test_modules() {
    std::unique_ptr<Embed::Isolate> isolate = Embed::Isolate::Create();
    std::unique_ptr<Embed::Realm> realm = isolate->CreateRealm();
    Embed::Realm& r = *realm;
    Context& ctx = r.GetContext();
    ModuleWorld world;
    install_module_world(*isolate, world);
    using S = Embed::ModuleSource;
    world.files["http://host/lib/a.js"] = S::Script("import { b } from './b.js'; export const a = 'a' + b; export function twice(f) { return f() + f(); }");
    world.files["http://host/lib/b.js"] = S::Script("export const b = 'b'; export let count = 0; export function bump() { count++; }");
    world.files["http://host/lib/data.json"] = S::Json("{\"k\": [1, 2]}");
    world.files["http://host/lib/note.txt"] = S::Text("hello");
    world.files["http://host/lib/blob.bin"] = S::Bytes({1, 2, 3});
    world.files["http://host/lib/broken.js"] = S::Script("export const x = ;");
    world.files["http://host/lib/throws.js"] = S::Script("\n\nthrow new Error('boom');");
    world.files["http://host/lib/tla.js"] = S::Script("export const v = await Promise.resolve(42);");
    world.files["http://host/lib/cycle1.js"] = S::Script("import { two } from './cycle2.js'; export function one() { return 1; } export const sum = () => one() + two();");
    world.files["http://host/lib/cycle2.js"] = S::Script("import { one } from './cycle1.js'; export function two() { return 2; } export const back = () => one();");
    world.files["http://host/lib/meta.js"] = S::Script("export default import.meta.url;");

    // A graph of static imports, the namespace of the entry, live bindings.
    Value p = r.EvaluateModule("import { a, twice } from './lib/a.js'; import { count, bump } from './lib/b.js';"
                               "bump(); bump(); globalThis.out = [a, twice(() => 2), count].join(',');", "http://host/main.js");
    Embed::ObjectInfo info = settled(*isolate, r, p);
    CHECK(info.promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, Value(ctx.get_global_object()), "out")) == "ab,4,2");
    CHECK(world.fetched.size() == 2);

    // The same URL is fetched once, however many modules ask.
    size_t before = world.fetched.size();
    p = r.EvaluateModule("import './lib/a.js'; import { b } from './lib/b.js'; globalThis.out2 = b;", "http://host/main2.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    CHECK(world.fetched.size() == before);

    // Typed imports.
    p = r.EvaluateModule("import j from './lib/data.json' with { type: 'json' };"
                         "import t from './lib/note.txt' with { type: 'text' };"
                         "import b from './lib/blob.bin' with { type: 'bytes' };"
                         "globalThis.typed = JSON.stringify(j) + t + b.length;", "http://host/main3.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, Value(ctx.get_global_object()), "typed")) == "{\"k\":[1,2]}hello3");

    // A host-made value is the default export as it is.
    world.files["http://host/lib/sheet.css"] = S::Default(Embed::NewString(ctx, "SHEET"));
    p = r.EvaluateModule("import s from './lib/sheet.css' with { type: 'css' }; globalThis.sheet = s;", "http://host/main4.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, Value(ctx.get_global_object()), "sheet")) == "SHEET");

    // Cycles, top-level await, import.meta.
    p = r.EvaluateModule("import { sum } from './lib/cycle1.js'; import { v } from './lib/tla.js';"
                         "import m from './lib/meta.js'; globalThis.mix = [sum(), v, m].join(',');", "http://host/main5.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, Value(ctx.get_global_object()), "mix")) == "3,42,http://host/lib/meta.js");

    // Failures say which module and where.
    p = r.EvaluateModule("import './lib/broken.js';", "http://host/e1.js");
    info = settled(*isolate, r, p);
    CHECK(info.promise_state == Embed::PromiseState::Rejected);
    Embed::ErrorInfo err = Embed::InspectError(ctx, info.promise_result);
    CHECK(err.name == "SyntaxError" && err.filename == "http://host/lib/broken.js" && err.line == 1);

    isolate->SetSourcePositionTracking(true);
    p = r.EvaluateModule("import './lib/throws.js';", "http://host/e2.js");
    info = settled(*isolate, r, p);
    err = Embed::InspectError(ctx, info.promise_result);
    CHECK(info.promise_state == Embed::PromiseState::Rejected && err.message == "boom" && err.line == 3);

    isolate->SetSourcePositionTracking(false);
    p = r.EvaluateModule("import './lib/missing.js';", "http://host/e3.js");
    info = settled(*isolate, r, p);
    CHECK(info.promise_state == Embed::PromiseState::Rejected);
    CHECK(Embed::InspectError(ctx, info.promise_result).message.find("404") != std::string::npos);
    size_t fetches = world.fetched.size();
    p = r.EvaluateModule("import './lib/missing.js';", "http://host/e4.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Rejected);
    CHECK(world.fetched.size() == fetches + 0);   // the failure is remembered

    p = r.EvaluateModule("import 'forbidden';", "http://host/e5.js");
    info = settled(*isolate, r, p);
    CHECK(info.promise_state == Embed::PromiseState::Rejected);
    CHECK(Embed::InspectError(ctx, info.promise_result).message == "no such package");

    // import() from a module, and from the host; the hook can refuse it.
    p = r.EvaluateModule("globalThis.dyn = import('./lib/b.js').then(m => m.b);", "http://host/d1.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    Embed::ObjectInfo dyn = settled(*isolate, r, Embed::Get(ctx, Value(ctx.get_global_object()), "dyn"));
    CHECK(dyn.promise_state == Embed::PromiseState::Fulfilled && Embed::ToWtf8(ctx, dyn.promise_result) == "b");
    world.deny_dynamic = true;
    p = r.EvaluateModule("globalThis.dyn2 = import('./lib/a.js');", "http://host/d2.js");
    settled(*isolate, r, p);
    dyn = settled(*isolate, r, Embed::Get(ctx, Value(ctx.get_global_object()), "dyn2"));
    CHECK(dyn.promise_state == Embed::PromiseState::Rejected);
    CHECK(Embed::InspectError(ctx, dyn.promise_result).message == "blocked: ./lib/a.js");
    world.deny_dynamic = false;
    Value imported = r.ImportModule("./a.js", "http://host/lib/x.js");
    Embed::ObjectInfo ns = settled(*isolate, r, imported);
    CHECK(ns.promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, ns.promise_result, "a")) == "ab");

    // A fetch that finishes later: the graph waits for it.
    world.files["http://host/lib/late.js"] = S::Script("export const late = 'L';");
    world.park = true;
    p = r.EvaluateModule("import { late } from './lib/late.js'; globalThis.got = late;", "http://host/l1.js");
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Pending);
    CHECK(world.parked.size() == 1);
    world.park = false;
    auto parked = std::move(world.parked);
    world.parked.clear();
    for (auto& [url, done] : parked) done(S());
    CHECK(settled(*isolate, r, p).promise_state == Embed::PromiseState::Fulfilled);
    CHECK(Embed::ToWtf8(ctx, Embed::Get(ctx, Value(ctx.get_global_object()), "got")) == "L");

    // A realm destroyed with a fetch still parked: answering it later is harmless.
    world.files["http://host/lib/never.js"] = S::Script("export const n = 1;");
    world.park = true;
    p = r.EvaluateModule("import './lib/never.js';", "http://host/l2.js");
    settled(*isolate, r, p);
    world.park = false;
    realm.reset();
    parked = std::move(world.parked);
    world.parked.clear();
    for (auto& [url, done] : parked) done(S());
    isolate->CollectGarbage();
}

int main() {
    // Freed cells are filled with a pattern and never reused, so a pointer a
    // test left behind into a dead runtime fails at its first use instead of
    // reading whatever was allocated there next. Read once, at the first sweep.
    setenv("QUANTA_GC_POISON", "1", 1);
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

    // Last: it destroys the runtime everything above used.
    rt.reset();
    test_sequential_runtimes();
    test_realms();
    test_realm_data();
    test_destroyed_realm_class();
    test_persistent();
    test_byte_buffers();
    test_record_order();
    test_globals_buffers_nested_proxies();
    test_legacy_platform_objects();
    test_lone_surrogates();
    test_arrow_arguments();
    test_accessor_redefined_in_setter();
    test_proxy_get_and_construct_reads();
    test_objects_and_conversions();
    test_buffers_zero_copy();
    test_realm_helpers();
    test_error_reporting();
    test_finalization_and_weak();
    test_inspect();
    test_source_positions();
    test_modules();

    std::printf("embed-test: %d checks, %d failed\n", g_checks, g_failures);
    // The heap is immortal by design; skip the static destructors that would
    // run against it.
    std::fflush(stdout);
    std::_Exit(g_failures == 0 ? 0 : 1);
}
