/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_CONTEXT_H
#define QUANTA_CONTEXT_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/SmallMapPool.h"
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <functional>

namespace Quanta {

class Engine;
class Function;
class StackFrame;
class Visitor;
class Environment;
class Error;

class Context {
public:
    enum class Type {
        Global,
        Function,
        Eval,
        Module
    };

    enum class State {
        Running,
        Suspended,
        Completed,
        Thrown
    };

private:
    Type type_;
    State state_;
    uint32_t context_id_;
    
    Environment* lexical_environment_;
    Environment* variable_environment_;
    Object* this_binding_;
    
    std::vector<std::unique_ptr<StackFrame>> call_stack_;
    
    mutable int execution_depth_;
    static const int max_execution_depth_ = 500;
    
    Object* global_object_;
    Context* builtins_root_ = nullptr;  // the global context owning the builtin maps (always outlives children)
    std::unordered_map<std::string, Object*> built_in_objects_;
    std::unordered_map<std::string, Function*> built_in_functions_;
    
    Value current_exception_;
    bool has_exception_;
    std::vector<std::pair<size_t, size_t>> try_catch_blocks_;

    Value return_value_;
    bool has_return_value_;

    bool has_break_;
    bool has_continue_;
    std::string break_label_;
    std::string continue_label_;
    std::string current_loop_label_;  // Track the label of the current loop
    std::string next_statement_label_;  // Label to be applied to the next statement

    bool is_in_constructor_call_;
    bool super_called_;
    bool this_needs_super_;  // derived class ctor: accessing 'this' before super() throws
    Object* last_super_override_ = nullptr;  // comparison-only, see last_super_override()
    Environment* owned_env_ = nullptr;  // see set_owned_env()
    // Set when a native call reused this exact context (Function.cpp's
    // is_native_ branch), or when this context was captured as a Generator's
    // outer_context_ / an AsyncFunction's Promise::context_ -- both bypass
    // Object::current_context_ entirely. Consulted by ContextSurvivorGuard:
    // together with owned_env_->is_escaped() (the closure-capture signal,
    // via Function::closure_context_/capture_closure_environment's
    // mark_escaped() call), this is the complete "could anything outlive this
    // call and still reach this context" answer -- see ContextSurvivorGuard's
    // doc comment.
    bool exposed_to_escape_ = false;
    Value new_target_;
    bool original_this_was_nullish_ = false;
    bool original_this_was_primitive_ = false; // set when native call had a non-null/undefined primitive thisArg
    // Set by Function::construct() right before it calls Function::call() on the same
    // function, so call() can tell "I'm the construct invocation" apart from a plain call
    // made from inside that constructor's body (which must see new.target == undefined).
    bool pending_construct_call_ = false;

    bool strict_mode_;
    
    Engine* engine_;
    
    std::string current_filename_;

    static thread_local uint32_t next_context_id_;

    // Microtask queue for Promise/async (only used on global context).
    // keep_alive lists every cell a task's lambda captures: closure storage
    // is invisible to the collector, the queue entry is its GC anchor.
    struct MicrotaskEntry {
        std::function<void()> task;
        std::vector<Value> keep_alive;
    };
    std::vector<MicrotaskEntry> microtask_queue_;
    std::vector<MicrotaskEntry> draining_queue_;  // batch in flight (traced too)
    bool in_param_eval_ = false;
    bool is_direct_eval_call_ = false;
    bool eval_arguments_conflict_ = false;
    std::unordered_set<std::string> eval_param_names_;
    bool is_arrow_function_context_ = false;
    bool in_class_field_init_ = false;
    Value import_meta_;

    // Dispose scope stack for 'using' declarations (Explicit Resource Management)
    struct DisposableResource {
        Value resource_value;     // passed as 'this' to dispose method
        Value dispose_method;     // looked up once at initialization time
        bool is_async_dispose;    // `await using` (vs `using`): Dispose() must Await() the call's result
    };
    std::vector<std::vector<DisposableResource>> dispose_scope_stack_;

public:
    void gc_trace(Visitor& v) const;

    explicit Context(Engine* engine, Type type = Type::Global);
    explicit Context(Engine* engine, Context* parent, Type type);
    ~Context();

    // Pooled: reuses freed blocks instead of round-tripping the allocator
    // on every call (see Context.cpp).
    static void* operator new(size_t size);
    static void operator delete(void* ptr);

    Type get_type() const { return type_; }
    State get_state() const { return state_; }
    uint32_t get_id() const { return context_id_; }
    Engine* get_engine() const { return engine_; }

    // Microtask queue (Promise async support)
    void queue_microtask(std::function<void()> task, std::vector<Value> keep_alive);
    void drain_microtasks();
    bool has_pending_microtasks() const { return !microtask_queue_.empty(); }
    bool is_in_param_eval() const { return in_param_eval_; }
    void set_in_param_eval(bool v) { in_param_eval_ = v; }
    bool is_direct_eval_call() const { return is_direct_eval_call_; }
    void set_direct_eval_call(bool v) { is_direct_eval_call_ = v; }
    bool is_arrow_function_context() const { return is_arrow_function_context_; }
    void set_arrow_function_context(bool v) { is_arrow_function_context_ = v; }
    bool has_eval_arguments_conflict() const { return eval_arguments_conflict_; }
    void set_eval_arguments_conflict(bool v) { eval_arguments_conflict_ = v; }
    const std::unordered_set<std::string>& get_eval_param_names() const { return eval_param_names_; }
    void set_eval_param_names(std::unordered_set<std::string> names) { eval_param_names_ = std::move(names); }
    bool is_in_class_field_init() const { return in_class_field_init_; }
    void set_in_class_field_init(bool v) { in_class_field_init_ = v; }
    Value get_import_meta();
    void set_import_meta(const Value& v) { import_meta_ = v; }
    
    const std::string& get_current_filename() const { return current_filename_; }
    void set_current_filename(const std::string& filename) { current_filename_ = filename; }
    
    bool is_strict_mode() const { return strict_mode_; }
    void set_strict_mode(bool strict) { strict_mode_ = strict; }
    bool original_this_was_nullish() const { return original_this_was_nullish_; }
    void set_original_this_nullish(bool v) { original_this_was_nullish_ = v; }
    bool original_this_was_primitive() const { return original_this_was_primitive_; }
    void set_original_this_primitive(bool v) { original_this_was_primitive_ = v; }

    Object* get_global_object() const { return global_object_; }
    void set_global_object(Object* global);

    Object* get_this_binding() const { return this_binding_; }
    void set_this_binding(Object* this_obj) { this_binding_ = this_obj; }

    Environment* get_lexical_environment() const { return lexical_environment_; }
    // The env created FOR this context (function/eval env); dies with the
    // context in ~Context unless a capture marked it escaped.
    void set_owned_env(Environment* env) { owned_env_ = env; }
    Environment* get_owned_env() const { return owned_env_; }
    void mark_exposed_to_escape() { exposed_to_escape_ = true; }
    bool exposed_to_escape() const { return exposed_to_escape_; }
    Environment* get_variable_environment() const { return variable_environment_; }
    void set_lexical_environment(Environment* env) { lexical_environment_ = env; }
    void set_variable_environment(Environment* env) { variable_environment_ = env; }
    
    void push_block_scope();
    void pop_block_scope();
    void push_with_scope(class Object* obj);
    void pop_with_scope();
    Environment* find_binding_env(const std::string& name) const;

    // Explicit Resource Management ('using' declaration support)
    void push_dispose_scope();
    void add_disposable_resource(const Value& resource, const Value& method, bool is_async_dispose = false);
    void run_dispose_resources();  // dispose current scope, pop it

    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    bool set_binding(const std::string& name, const Value& value);
    std::unordered_map<std::string, Value> snapshot_bindings() const;
    void restore_bindings(const std::unordered_map<std::string, Value>& snapshot);
    bool is_lexical_const(const std::string& name) const;
    bool is_strict_const(const std::string& name) const;
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true, bool deletable = true, bool enumerable = true);
    void create_binding_force(const std::string& name, const Value& value);
    void create_lexical_binding_force(const std::string& name, const Value& value);
    bool create_var_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool create_lexical_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    void create_global_function_binding(const std::string& name, const Value& value, bool configurable = false);
    bool delete_binding(const std::string& name);
    bool is_in_tdz(const std::string& name) const;

    void push_frame(std::unique_ptr<StackFrame> frame);
    std::unique_ptr<StackFrame> pop_frame();
    StackFrame* current_frame() const;
    size_t stack_depth() const { return call_stack_.size(); }
    bool is_stack_overflow() const { return stack_depth() > 10000; }
    
    bool check_execution_depth() const;
    void increment_execution_depth() const { execution_depth_++; }
    void decrement_execution_depth() const { execution_depth_--; }

    bool has_exception() const { return has_exception_; }
    const Value& get_exception() const { return current_exception_; }
    void throw_exception(const Value& exception, bool raw = false);
    void clear_exception();
    
    void throw_error(const std::string& message);
    void throw_type_error(const std::string& message);
    void throw_reference_error(const std::string& message);
    void throw_syntax_error(const std::string& message);
    void throw_range_error(const std::string& message);
    void throw_uri_error(const std::string& message);
    
    bool has_return_value() const { return has_return_value_; }
    const Value& get_return_value() const { return return_value_; }
    void set_return_value(const Value& value);
    void clear_return_value();
    
    bool has_break() const { return has_break_; }
    bool has_continue() const { return has_continue_; }
    const std::string& get_break_label() const { return break_label_; }
    const std::string& get_continue_label() const { return continue_label_; }
    void set_break(const std::string& label = "");
    void set_continue(const std::string& label = "");
    void clear_break_continue();

    const std::string& get_current_loop_label() const { return current_loop_label_; }
    void set_current_loop_label(const std::string& label) { current_loop_label_ = label; }

    const std::string& get_next_statement_label() const { return next_statement_label_; }
    void set_next_statement_label(const std::string& label) { next_statement_label_ = label; }

    bool is_in_constructor_call() const { return is_in_constructor_call_; }
    void set_in_constructor_call(bool value) { is_in_constructor_call_ = value; }
    bool was_super_called() const { return super_called_; }
    void set_super_called(bool value) { super_called_ = value; }
    bool this_needs_super() const { return this_needs_super_; }
    void set_this_needs_super(bool v) { this_needs_super_ = v; }
    // Identity of the object super() swapped `this` to (return-override), used
    // only for pointer comparison in Function::construct -- never dereferenced,
    // so it needs no GC trace.
    Object* last_super_override() const { return last_super_override_; }
    void set_last_super_override(Object* o) { last_super_override_ = o; }

    Value get_new_target() const { return new_target_; }
    void set_new_target(const Value& val) { new_target_ = val; }

    void set_pending_construct_call(bool v) { pending_construct_call_ = v; }
    bool consume_pending_construct_call() {
        bool v = pending_construct_call_;
        pending_construct_call_ = false;
        return v;
    }

    void register_built_in_object(const std::string& name, Object* object);
    void register_built_in_function(const std::string& name, Function* function);
    Object* get_built_in_object(const std::string& name) const;
    Function* get_built_in_function(const std::string& name) const;

    void suspend() { state_ = State::Suspended; }
    void resume() { state_ = State::Running; }
    void complete() { state_ = State::Completed; }

    std::string get_stack_trace() const;
    std::vector<std::string> get_variable_names() const;
    std::string debug_string() const;

    void mark_references() const;

    // Garbage collector access

    // Bootstrap loading
    void load_bootstrap();

    // Releases ownership; the cell lives until the collector proves it dead.
    template<typename T>
    T* track(std::unique_ptr<T> obj) {
        return obj.release();
    }

private:
    void initialize_global_context();
    void initialize_built_ins();
    void setup_test262_helpers();
    void setup_global_bindings();
    void register_typed_array_constructors();
};

/**
 * Stack frame for function calls
 */
class StackFrame {
public:
    enum class Type {
        Script,
        Function,
        Constructor,
        Method,
        Eval,
        Native
    };

private:
    Type type_;
    Function* function_;
    Object* this_binding_;
    std::vector<Value> arguments_;
    std::unordered_map<std::string, Value> local_variables_;
    Environment* environment_;
    
    size_t program_counter_;
    std::string source_location_;
    uint32_t line_number_;
    uint32_t column_number_;

public:
    void gc_trace(Visitor& v) const;

    StackFrame(Type type, Function* function, Object* this_binding);
    ~StackFrame() = default;

    Type get_type() const { return type_; }
    Function* get_function() const { return function_; }
    Object* get_this_binding() const { return this_binding_; }
    Environment* get_environment() const { return environment_; }

    void set_arguments(const std::vector<Value>& args) { arguments_ = args; }
    const std::vector<Value>& get_arguments() const { return arguments_; }
    size_t argument_count() const { return arguments_.size(); }
    Value get_argument(size_t index) const;

    bool has_local(const std::string& name) const;
    Value get_local(const std::string& name) const;
    void set_local(const std::string& name, const Value& value);

    size_t get_program_counter() const { return program_counter_; }
    void set_program_counter(size_t pc) { program_counter_ = pc; }
    
    void set_source_location(const std::string& location, uint32_t line, uint32_t column);
    std::string get_source_location() const { return source_location_; }
    uint32_t get_line_number() const { return line_number_; }
    uint32_t get_column_number() const { return column_number_; }

    std::string to_string() const;
};

/**
 * Environment for variable bindings
 */
class Environment {
public:
    enum class Type {
        Declarative,
        Object,
        Function,
        Module,
        Global
    };

public:
    // A binding used to be spread across 4 parallel maps (bindings_/
    // mutable_flags_/initialized_flags_/deletable_flags_), hashing and
    // looking up the same key up to 4 times per binding creation. Default
    // member initializers mirror each old map's "key absent" fallback
    // (is_mutable_binding/is_initialized_binding/the deletable check all
    // read this way) -- get these wrong and a binding whose flags were never
    // explicitly set (see initialize_binding) silently gets the wrong ones.
    struct BindingSlot {
        Value value;
        bool mutable_flag = true;   // is_mutable_binding: absent -> mutable
        bool initialized = false;   // is_initialized_binding: absent -> not yet
        bool deletable = false;     // ES1 DontDelete: absent -> not deletable
    };

    // Like HybridDescriptorMap's inline array (Object.h), but can't copy its
    // migrate-to-overflow-when-full step: stable_binding_slot() hands out a
    // raw Value* that BytecodeChunk::lookup_cache and
    // Function::instance_lookup_cache_ cache PERMANENTLY (Op::LdaLookup/
    // StaLookup trust it for the owning chunk/Function's whole lifetime), and
    // migration would silently invalidate that pointer. So: inline and
    // overflow entries, once populated, NEVER move. Erasing an inline entry
    // tombstones it in place instead of compacting (compaction would relocate
    // a survivor). Tombstones ARE reused by later inserts -- safe because
    // stable_binding_slot() refuses deletable bindings, the only kind that
    // can ever be erased, so a tombstoned slot never has a live cached
    // pointer. Re-audit lookup_cache/instance_lookup_cache_ before relaxing
    // any of this.
    struct SlotMap {
        static constexpr size_t kInlineCapacity = 4;
        struct InlineEntry {
            std::string key;
            BindingSlot slot;
            bool in_use = false;
        };
        std::array<InlineEntry, kInlineCapacity> inline_entries;
        using OverflowMap = std::unordered_map<std::string, BindingSlot, std::hash<std::string>,
                                                std::equal_to<std::string>,
                                                SmallMapAllocator<std::pair<const std::string, BindingSlot>>>;
        std::unique_ptr<OverflowMap> overflow;

        BindingSlot* find(const std::string& name) {
            for (auto& e : inline_entries) {
                if (e.in_use && e.key == name) return &e.slot;
            }
            if (overflow) {
                auto it = overflow->find(name);
                if (it != overflow->end()) return &it->second;
            }
            return nullptr;
        }
        const BindingSlot* find(const std::string& name) const {
            return const_cast<SlotMap*>(this)->find(name);
        }

        // Insert-if-absent-then-return-reference, mirroring unordered_map::
        // operator[]'s semantics (the call sites all rely on this).
        BindingSlot& get_or_create(const std::string& name) {
            if (BindingSlot* existing = find(name)) return *existing;
            for (auto& e : inline_entries) {
                if (!e.in_use) {
                    e.key = name;
                    e.slot = BindingSlot{};
                    e.in_use = true;
                    return e.slot;
                }
            }
            if (!overflow) overflow = std::make_unique<OverflowMap>();
            return (*overflow)[name];
        }

        // Tombstones (never compacts -- see class doc comment). Overflow
        // erase is ordinary unordered_map::erase: safe unchanged, since
        // erasing one node never moves another node's address.
        bool erase(const std::string& name) {
            for (auto& e : inline_entries) {
                if (e.in_use && e.key == name) {
                    e.in_use = false;
                    e.slot = BindingSlot{};
                    e.key.clear();
                    return true;
                }
            }
            return overflow && overflow->erase(name) > 0;
        }

        size_t size() const {
            size_t n = 0;
            for (const auto& e : inline_entries) if (e.in_use) n++;
            if (overflow) n += overflow->size();
            return n;
        }

        template <typename Fn>
        void for_each(Fn&& fn) const {
            for (const auto& e : inline_entries) {
                if (e.in_use) fn(e.key, e.slot);
            }
            if (overflow) {
                for (const auto& kv : *overflow) fn(kv.first, kv.second);
            }
        }
    };

private:
    Type type_;
    Environment* outer_environment_;
    SlotMap slots_;
    std::unordered_set<std::string> lexical_names_;
    std::unordered_set<std::string> const_binding_names_; // tracks const declarations in Object envs
    Object* binding_object_;
    bool is_with_environment_ = false; // ES6 8.1.1.2.1 HasBinding: only `with` object environments consult @@unscopables
    bool is_closure_boundary_ = false; // marks script-level env: stop snapshot loops here
    bool escaped_ = false;  // see is_escaped()

public:
    // Write-barrier dedup flag, owned by the Collector (set on first binding
    // write per GC cycle, cleared after the cycle).
    bool gc_remembered_ = false;

    void gc_trace(Visitor& v) const;

    Environment(Type type, Environment* outer = nullptr);
    Environment(Object* binding_object, Environment* outer = nullptr);
    ~Environment() = default;

    // Pooled: reuses freed blocks instead of round-tripping the allocator
    // on every call (see Context's identical pattern in Context.cpp).
    static void* operator new(size_t size);
    static void operator delete(void* ptr);

    Type get_type() const { return type_; }
    Environment* get_outer() const { return outer_environment_; }
    Object* get_binding_object() const { return binding_object_; }
    bool is_with_environment() const { return is_with_environment_; }
    void set_with_environment(bool value) { is_with_environment_ = value; }
    bool is_closure_boundary() const { return is_closure_boundary_; }
    void mark_closure_boundary() { is_closure_boundary_ = true; }

    // Escape tracking: a block-scope env that was never captured (by a closure's
    // closure_environment_, a child context's outer chain, or an eval env) can be
    // deleted on pop instead of leaking. Marking walks the outer chain so every
    // env reachable from a captured one is pinned too.
    bool is_escaped() const { return escaped_; }
    void mark_escaped() {
        for (Environment* e = this; e && !e->escaped_; e = e->outer_environment_) {
            e->escaped_ = true;
        }
    }

    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    Value get_binding_with_depth(const std::string& name, int depth) const;
    bool set_binding(const std::string& name, const Value& value);
    Value get_binding_direct(const std::string& name, Context* ctx = nullptr) const;
    // Address-stable storage pointer for an initialized, mutable,
    // non-deletable declarative binding (unordered_map nodes never move).
    // Backbone of the VM's outer-variable cache; null when any guard fails.
    Value* stable_binding_slot(const std::string& name);
    // Guarded direct-index access to slots_'s inline array, backing
    // Op::LdaEnvSlot/StaEnvSlot/StaEnvSlotInit. The compiler's predicted
    // index can be wrong (see BytecodeCompiler.h's EnvSlotInfo for why), so
    // this re-validates by name: returns null unless inline_entries[index]
    // is in_use AND its key equals `name`. A null return means "fall back
    // to the name-based path," never "this binding doesn't exist."
    SlotMap::InlineEntry* inline_slot(size_t index, const std::string& name) {
        if (index >= SlotMap::kInlineCapacity) return nullptr;
        SlotMap::InlineEntry& e = slots_.inline_entries[index];
        if (e.in_use && e.key == name) return &e;
        return nullptr;
    }
    bool set_binding_direct(const std::string& name, const Value& value, Context* ctx = nullptr);
    Environment* find_binding_env(const std::string& name);
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true, bool deletable = true, bool enumerable = true);
    void force_set_binding(const std::string& name, const Value& value);
    bool delete_binding(const std::string& name);

    bool is_mutable_binding(const std::string& name) const;
    bool has_mutable_flag(const std::string& name) const;
    bool is_initialized_binding(const std::string& name) const;
    void initialize_binding(const std::string& name, const Value& value);

    std::vector<std::string> get_binding_names() const;
    std::string debug_string() const;

    void mark_references() const;

    bool has_own_binding(const std::string& name) const;
    bool has_lexical_declaration(const std::string& name) const { return lexical_names_.count(name) > 0; }
    void mark_lexical_declaration(const std::string& name) { lexical_names_.insert(name); }
    bool is_const_binding(const std::string& name) const { return const_binding_names_.count(name) > 0; }
    void mark_const_binding(const std::string& name) { const_binding_names_.insert(name); }
    void create_global_function_binding(const std::string& name, const Value& value, bool configurable = false);
    void create_uninitialized_binding(const std::string& name, bool is_mutable = true);
};

// Suspends the current async fiber until `value` settles (mirrors AwaitExpression::evaluate);
// returns true and sets out_result to the rejection reason if it rejected, else the fulfilled value.
bool await_value(Context& ctx, const Value& value, Value& out_result);

/**
 * Context factory for creating specialized contexts
 */
namespace ContextFactory {
    std::unique_ptr<Context> create_global_context(Engine* engine);
    std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function);
    std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent);
    std::unique_ptr<Context> create_module_context(Engine* engine);
}

// RAII guard: on scope exit, transfers a still-owned function context to the
// Engine's survivor pool instead of letting its unique_ptr free it.
//
// A closure created anywhere inside the call (e.g. a default-parameter
// expression, even one that never finishes because it throws) captures this
// context as its Function::closure_context_. That closure is a GC cell whose
// lifetime is decided by reachability, not by this call's C++ stack frame --
// if the call takes an early-return/abrupt-completion path, deleting the
// context out from under a closure that already escaped is a dangling
// reference the next collection cycle will read.
//
// A no-op when the ptr has already been moved elsewhere (the ordinary
// success path transfers ownership into the created Function/Generator
// object itself, which is a proper GC cell -- no separate pinning needed).
//
// Also a no-op (context destructs normally, no pool registration) when
// NOTHING could have captured a reference to it: no closure captured its
// owned_env_ (checked via Environment::is_escaped(), the same signal
// capture_closure_environment()/Function::set_closure_environment() already
// set whenever this context becomes a Function::closure_context_), AND
// nothing marked it exposed_to_escape() (native calls reusing this exact
// context, or this context becoming a Generator's outer_context_ / an
// AsyncFunction's Promise::context_). If a future change ever stores a
// Context* into a longer-lived structure through some OTHER path than these,
// it must also call mark_exposed_to_escape() or this skip becomes unsafe.
struct ContextSurvivorGuard {
    std::unique_ptr<Context>& ptr;
    Engine* eng;
    ContextSurvivorGuard(std::unique_ptr<Context>& p, Engine* e) : ptr(p), eng(e) {}
    // Defined in Context.cpp: Engine is only forward-declared here.
    ~ContextSurvivorGuard();
};

}

#endif
