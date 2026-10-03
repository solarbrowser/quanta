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

    std::printf("embed-test: %d checks, %d failed\n", g_checks, g_failures);
    // The heap is immortal by design; skip the static destructors that would
    // run against it.
    std::fflush(stdout);
    std::_Exit(g_failures == 0 ? 0 : 1);
}
