/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_H
#define QUANTA_ENGINE_H

#include "quanta/core/runtime/Value.h"
#include <span>
#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Realm.h"
#include "quanta/core/engine/Isolate.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/engine/StructuredClone.h"
#include "quanta/core/gc/Heap.h"
#include "quanta/parser/AST.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace Quanta {

class ASTNode;

class Engine {
public:
    struct Config {
        bool strict_mode = false;
        bool enable_optimizations = true;
        size_t max_heap_size = 512 * 1024 * 1024;
        size_t initial_heap_size = 32 * 1024 * 1024;
        size_t max_stack_size = 8 * 1024 * 1024;
        bool enable_debugger = false;
        bool enable_profiler = false;
        // $262 (createRealm/detachArrayBuffer/evalScript/gc) is a test-harness
        // interface, not something arbitrary script should ever see -- a real
        // embedder (Solar Browser) must not expose realm creation or forced
        // GC to web content. Off by default; only a test262-running host
        // (console.cpp's --test262) turns it on.
        bool expose_test262_globals = false;
        // The CLI runs a script and then keeps going until every timer has
        // fired, sleeping as long as that takes. A host with an event loop of
        // its own cannot be blocked like that: with this set, running a script
        // drains the microtask queue and stops, and the host fires timers
        // itself (EventLoop::run_due_timers) when its own loop says to.
        bool host_drives_event_loop = false;
        // Quanta's own console (log, error, warn on stdout/stderr). A host that implements
        // console itself leaves it out, so there is no global of it to overwrite.
        bool install_console = true;
    };

    struct Result {
        Value value;
        Value exception_value; 
        bool success;
        std::string error_message;
        uint32_t line_number;
        uint32_t column_number;

        Result() : success(false), line_number(0), column_number(0) {}
        Result(const Value& v) : value(v), success(true), line_number(0), column_number(0) {}
        Result(const std::string& error, uint32_t line = 0, uint32_t col = 0)
            : success(false), error_message(error), line_number(line), column_number(col) {}
        Result(const std::string& error, const Value& exc, uint32_t line = 0, uint32_t col = 0)
            : exception_value(exc), success(false), error_message(error), line_number(line), column_number(col) {}
    };

private:
    Config config_;
    // Intentionally immortal for now (like realm engines): static
    // destructors delete cells after the engine dies, so the heap and its
    // metadata must stay valid until process exit. The collector's shutdown
    // protocol will make heaps destructible.
    Isolate* isolate_;
    void* host_realm_ = nullptr;
    Heap* heap_;
    // This engine's intrinsics; see Realm.
    std::unique_ptr<Realm> realm_;
    std::unique_ptr<Context> global_context_;
    std::unique_ptr<ModuleLoader> module_loader_;
    std::shared_ptr<ModuleHost> module_host_;
    std::shared_ptr<SerializationHost> serialization_host_;

    bool initialized_;
    uint64_t execution_count_;

    // Survivor pool: function contexts kept alive until after microtask drain,
    // so Promise callbacks can use context_ (creation context) for closure lookups
    std::vector<Context*> survivor_contexts_;
    // Same pattern for escaped Environments whose owning Context/block scope
    // already exited (see Collector::finish_major_cycle's environment-survivor
    // prune loop) -- without this, an escaped Environment (any closure that
    // captures an outer variable) would never be freed at all.
    std::vector<Environment*> survivor_environments_;

    // Head of the chain of ExecContextScopes currently open on this engine.
    // Kept here rather than in a thread-local because every site that opens
    // one already holds the engine: reaching a thread-local from another
    // translation unit costs more than the push it guards, and reaching it
    // through a call forces the hot caller to spill around it.
    class ExecContextScope* exec_top_scope_ = nullptr;
    
    std::unordered_map<std::string, Value> default_exports_registry_;
    
    std::chrono::high_resolution_clock::time_point start_time_;
    size_t total_allocations_;
    size_t total_gc_runs_;

public:
    // An Engine of its own: it makes an Isolate (and so a heap) that nothing else
    // shares and that, like the engine, is never released.
    Engine();
    explicit Engine(const Config& config);
    // A realm in an existing Isolate, sharing its heap with the others in it.
    Engine(Isolate& isolate, const Config& config);
    ~Engine();

    bool initialize();
    void shutdown();
    bool is_initialized() const { return initialized_; }

    Result execute(const std::string& source);
    Result execute(const std::string& source, const std::string& filename);
    // Takes the caller's buffer rather than copying it. A script's text is
    // held for as long as the parse tree that addresses it, so the caller
    // that read it from disk can hand it over instead of keeping one each.
    Result execute(std::shared_ptr<const std::string> source, const std::string& filename);
    Result execute_file(const std::string& filename);
    
    Result evaluate(const std::string& expression, bool strict_mode = false);
    
    Result load_module(const std::string& module_path);
    Result import_module(const std::string& module_name);
    
    void set_global_property(const std::string& name, const Value& value);
    Value get_global_property(const std::string& name);
    bool has_global_property(const std::string& name);
    
    void register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func);
    void register_object(const std::string& name, Object* object);
    
    Context* get_global_context() const { return global_context_.get(); }
    const std::vector<Context*>& get_survivor_contexts() const { return survivor_contexts_; }
    // Direct access for Collector::run_collection's reachability-based prune
    // (a major cycle rewrites this in place once it knows which survivors
    // are still actually reachable -- see its doc comment).
    std::vector<Context*>& mutable_survivor_contexts() { return survivor_contexts_; }
    // Every live engine on this thread, for GC root enumeration (a
    // collection only ever scans the calling thread's own engines/heaps).
    static const std::vector<Engine*>& all_engines();
    // What the engine caches per thread about the realm it built (the iterator,
    // generator, Map/Set and primitive prototypes, %ThrowTypeError%, the job
    // queue, ...) is a pointer into that realm's cells. Run when the last engine
    // on the thread is destroyed: a new engine must start from nothing, not from
    // a dead realm's prototypes -- the capture-once ones keep whatever is
    // already there, and the heap sweeps the old cells.
    static void release_thread_realm_state();
    Context* get_current_context() const;

    // GetFunctionRealm(fn), spec 27.2.4 -- which realm `fn` belongs to, for
    // GetPrototypeFromConstructor's fallback when new.target's own
    // "prototype" isn't an object: the spec says use *that realm's* intrinsic
    // default, not whichever realm happens to be running right now. A native
    // constructor's closure_context_ is always null (see Function's own
    // constructors), so realm identity has to be found another way: each
    // realm's own %Function.prototype% is a distinct object (createRealm
    // never shares one), so whichever live engine's %Function.prototype%
    // equals fn's own [[Prototype]] is the one that made fn. Walks
    // all_engines() -- a linear scan, but only reached on this already-rare
    // fallback path, never on an ordinary construction where new.target's own
    // "prototype" is already an object.
    static Context* find_realm_owning_function(Object* fn);
    // realm's own named global constructor's "prototype" (e.g. "Boolean" ->
    // that realm's %BooleanPrototype%) -- nullptr if realm is null or the
    // name isn't bound to a constructor there.
    static Object* get_realm_intrinsic_prototype(Context* realm, const std::string& ctor_name);
    // The two combined: nullptr on any failure, so a caller's existing
    // default_proto fallback still applies unchanged.
    static Object* realm_intrinsic_prototype_for(Object* new_target_like, const std::string& ctor_name);

    // Same idea as find_realm_owning_function, generalized to any object
    // whose OWN [[Prototype]] (not necessarily itself callable) should
    // identity-match some realm's named intrinsic's own "prototype" -- e.g.
    // an Array instance against every live engine's own %Array.prototype%.
    // Used to recover "which realm made this" for a plain object with no
    // closure_context_ of its own (a collection instance, not a function).
    static Context* find_realm_owning_object(Object* obj, const std::string& ctor_name) {
        if (!obj) return nullptr;
        Object* proto = obj->get_prototype();
        if (!proto) return nullptr;
        for (Engine* e : all_engines()) {
            Context* gctx = e->get_global_context();
            if (get_realm_intrinsic_prototype(gctx, ctor_name) == proto) return gctx;
        }
        return nullptr;
    }

    // Iterator Helper result objects' own [[Prototype]] is NOT %Iterator.prototype%
    // (what find_realm_owning_object(obj, "Iterator") would check, via "Iterator"'s
    // own exposed .prototype property) -- it's Iterator::s_iterator_prototype_(),
    // a separate object that register_iterator_helpers installs map/filter/take/
    // drop/etc. directly onto, which array/string/map/set iterators (and every
    // Iterator Helper's own next()-result) reach before ever reaching %Iterator.
    // prototype%. Confirmed empirically: [].values().drop(0)'s own [[Prototype]]
    // is NOT Iterator.prototype (a distinct, one-level-further-up object).
    // Each realm's own instance is retrievable via its "@@IteratorPrototype"
    // context binding (Iterator::setup_iterator_prototype's own doc comment).
    static Object* get_realm_iterator_helper_prototype(Context* realm) {
        if (!realm) return nullptr;
        Value v = realm->get_binding("@@IteratorPrototype");
        return v.is_object() ? v.as_object() : nullptr;
    }
    static Context* find_realm_owning_iterator_helper(Object* obj) {
        if (!obj) return nullptr;
        Object* proto = obj->get_prototype();
        if (!proto) return nullptr;
        for (Engine* e : all_engines()) {
            Context* gctx = e->get_global_context();
            if (get_realm_iterator_helper_prototype(gctx) == proto) return gctx;
        }
        return nullptr;
    }

    // A just-created, not-yet-escaped object/array literal (ObjectFactory::
    // create_object()/create_array()) was stamped with a thread_local "last
    // realm set up" default prototype -- correct for the single/no-realm
    // case (the overwhelming majority) but wrong once 2+ realms exist, since
    // ObjectCreate/ArrayCreate must use the CURRENT realm's own %Object.
    // prototype%/%Array.prototype%. Repoints it in place when needed; a
    // no-op single vector-size check otherwise. `ctx` is the running
    // context at the literal's creation site (e.g. VM::run's own Frame::ctx).
    static void fixup_new_object_realm(Object* obj, Context* ctx) {
        if (!obj || !ctx || all_engines().size() <= 1) return;
        if (Object* realm_proto = get_realm_intrinsic_prototype(ctx, "Object")) {
            obj->initialize_prototype_of_new(realm_proto);
        }
    }
    static void fixup_new_array_realm(Object* obj, Context* ctx) {
        if (!obj || !ctx || all_engines().size() <= 1) return;
        if (Object* realm_proto = get_realm_intrinsic_prototype(ctx, "Array")) {
            obj->initialize_prototype_of_new(realm_proto);
        }
    }
    // Same idea, for ObjectFactory::create_native_function's own %Function.
    // prototype% stamping (get_function_prototype(), Function.cpp), which
    // has the identical thread_local-"last realm set up" default -- not
    // covered by the create_object()/create_array() audit this comment's
    // siblings were written for, since a native FUNCTION is a different
    // factory path. Needed wherever a native function is built explicitly
    // for a specific OTHER realm than whichever one last ran its own setup
    // (e.g. ShadowRealm's WrappedFunctionCreate).
    static void fixup_new_function_realm(Object* fn, Context* ctx) {
        if (!fn || !ctx || all_engines().size() <= 1) return;
        if (Object* realm_proto = get_realm_intrinsic_prototype(ctx, "Function")) {
            fn->initialize_prototype_of_new(realm_proto);
        }
    }

    // Survivor pool for function contexts (Promise async support). Pruned
    // only by the collector's own reachability-based pass (Collector.cpp) --
    // a context not currently in EventLoop use may still be reachable
    // through a closure, which only a mark pass can confirm.
    class ExecContextScope* exec_top_scope() const { return exec_top_scope_; }
    void set_exec_top_scope(class ExecContextScope* s) { exec_top_scope_ = s; }

    void add_survivor_context(Context* ctx);
    // The realm is being destroyed while others share its Isolate: gives what
    // still lingers of it to `heir`.
    void retire_into(Engine& heir);

    // Same pattern for escaped Environments (see survivor_environments_).
    const std::vector<Environment*>& get_survivor_environments() const { return survivor_environments_; }
    std::vector<Environment*>& mutable_survivor_environments() { return survivor_environments_; }
    void add_survivor_environment(Environment* env);

    // Drains microtasks, then drives any pending timers to exhaustion (real wall-clock wait), repeating until both queues are empty.
    void run_event_loop_to_completion(Context& ctx);
    
    size_t get_heap_usage() const;
    size_t get_heap_size() const;
    void force_gc();
    Heap* get_heap() const { return heap_; }
    // What the embedder keeps to find its own object for this realm (Embed::Realm).
    void* host_realm() const { return host_realm_; }
    void set_host_realm(void* host_realm) { host_realm_ = host_realm; }
    Isolate* isolate() const { return isolate_; }
    Realm* realm() const { return realm_.get(); }
    
    void enable_profiler(bool enable);
    void enable_debugger(bool enable);
    std::string get_performance_stats() const;
    std::string get_memory_stats() const;
    
    
    void set_error_handler(std::function<void(const std::string&)> handler);
    bool has_pending_exception() const;
    Value get_pending_exception() const;
    void clear_pending_exception();
    
    const Config& get_config() const { return config_; }
    void update_config(const Config& config);

    ModuleLoader* get_module_loader() { return module_loader_.get(); }
    // How this realm's modules are named, fetched and decorated by the host (see ModuleHost). Null: the
    // file system, as the CLI does.
    void set_module_host(std::shared_ptr<ModuleHost> host) { module_host_ = std::move(host); }
    // Where structured clone hands host objects; none means they cannot be cloned.
    SerializationHost* serialization_host() const { return serialization_host_.get(); }
    void set_serialization_host(std::shared_ptr<SerializationHost> host) { serialization_host_ = std::move(host); }
    ModuleHost* module_host() const { return module_host_.get(); }
    
    void register_default_export(const std::string& filename, const Value& value);
    Value get_default_export(const std::string& filename);
    bool has_default_export(const std::string& filename);

private:
    void setup_global_object();
    void setup_built_in_objects();
    void setup_built_in_functions();
    void setup_error_types();
    void setup_minimal_globals();
    
    Result execute_internal(std::shared_ptr<const std::string> source, const std::string& filename);
    
    void handle_exception(const Value& exception);
};

/**
 * Simplified function wrapper for native functions
 */
class NativeFunction {
public:
    using FunctionType = std::function<Value(Context&, std::span<const Value>, Value receiver)>;
    
private:
    FunctionType function_;
    std::string name_;
    size_t arity_;

public:
    NativeFunction(const std::string& name, FunctionType func, size_t arity = 0);
    
    Value call(Context& ctx, std::span<const Value> args, Value receiver);
    const std::string& get_name() const { return name_; }
    size_t get_arity() const { return arity_; }
};

/**
 * Engine factory for different configurations
 */
// Registers a Context as a collection root for as long as a call is running
// on it. A Context is not a GC cell, so the conservative stack scan cannot
// reach the values it holds -- something has to name it explicitly.
//
// The link lives in the scope object itself, which is already a stack local
// at every site, so neither Context nor Engine grows a per-call field, and
// the head is reached through an engine pointer the caller already holds.
class ExecContextScope {
public:
    explicit ExecContextScope(Context* ctx)
        : ctx_(ctx), engine_(ctx->get_engine()) {
        prev_ = engine_->exec_top_scope();
        engine_->set_exec_top_scope(this);
    }

    // A call returns before its caller, so the scope being closed is almost
    // always the head. A fiber suspended mid-call and resumed from another
    // stack is the exception, and unlinks from the middle instead.
    ~ExecContextScope() {
        if (engine_->exec_top_scope() == this) {
            engine_->set_exec_top_scope(prev_);
            return;
        }
        unlink();
    }

    Context* context() const { return ctx_; }
    ExecContextScope* prev() const { return prev_; }

    ExecContextScope(const ExecContextScope&) = delete;
    ExecContextScope& operator=(const ExecContextScope&) = delete;

private:
    void unlink() {
        for (ExecContextScope* s = engine_->exec_top_scope(); s; s = s->prev_) {
            if (s->prev_ == this) { s->prev_ = prev_; return; }
        }
    }

    Context* ctx_;
    Engine* engine_;
    ExecContextScope* prev_;
};

namespace EngineFactory {
    std::unique_ptr<Engine> create_browser_engine();
    std::unique_ptr<Engine> create_server_engine();
    std::unique_ptr<Engine> create_embedded_engine();
    std::unique_ptr<Engine> create_testing_engine();
}

}

#endif
