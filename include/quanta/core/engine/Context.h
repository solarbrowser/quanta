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

    // Packed into one bit-field group instead of scattered among the
    // pointer/Value/container fields below -- measured via mirror struct to
    // beat scattering (each bool otherwise pads out to share a stranded
    // byte with whatever 8-byte-aligned field follows it) by 56 bytes total,
    // and beat grouping at the end of the class instead of here by a
    // further 8 bytes.
    bool has_exception_ : 1 = false;
    bool has_return_value_ : 1 = false;
    bool has_break_ : 1 = false;
    bool has_continue_ : 1 = false;
    bool is_in_constructor_call_ : 1 = false;
    bool super_called_ : 1 = false;
    bool this_needs_super_ : 1 = false;  // derived class ctor: accessing 'this' before super() throws
    // Set when a native call reused this exact context (Function.cpp's
    // is_native_ branch), or when this context was captured as a Generator's
    // outer_context_ / an AsyncFunction's Promise::context_ -- both bypass
    // Object::current_context_ entirely. Consulted by ContextSurvivorGuard:
    // together with owned_env_->is_escaped() (the closure-capture signal,
    // via Function::closure_context_/capture_closure_environment's
    // mark_escaped() call), this is the complete "could anything outlive this
    // call and still reach this context" answer -- see ContextSurvivorGuard's
    // doc comment.
    bool exposed_to_escape_ : 1 = false;
    bool original_this_was_nullish_ : 1 = false;
    bool original_this_was_primitive_ : 1 = false; // set when native call had a non-null/undefined primitive thisArg
    // Set by Function::construct() right before it calls Function::call() on the same
    // function, so call() can tell "I'm the construct invocation" apart from a plain call
    // made from inside that constructor's body (which must see new.target == undefined).
    bool pending_construct_call_ : 1 = false;
    bool strict_mode_ : 1 = false;
    bool in_param_eval_ : 1 = false;
    bool is_direct_eval_call_ : 1 = false;
    bool eval_arguments_conflict_ : 1 = false;
    bool is_arrow_function_context_ : 1 = false;
    bool in_class_field_init_ : 1 = false;

    Environment* lexical_environment_;
    Environment* variable_environment_;
    Value this_value_;

    mutable int execution_depth_;
    static const int max_execution_depth_ = 500;

    Object* global_object_;
    Context* builtins_root_ = nullptr;  // the global context owning the builtin maps (always outlives children)
    // Only ever populated on the one Context register_built_in_object()/
    // register_built_in_function() are actually called on (bootstrap, on
    // the global context) -- every other Context resolves through
    // builtins_root_ instead (get_built_in_object()/get_built_in_function()
    // below) and never touches its own copy, so it stays null forever.
    struct BuiltinMaps {
        std::unordered_map<std::string, Object*> objects;
        std::unordered_map<std::string, Function*> functions;
    };
    std::unique_ptr<BuiltinMaps> builtins_;

    Value current_exception_;

    Value return_value_;

    // Lazy, bundled: entirely dead for VM-compiled code (only the legacy
    // tree-walking evaluator touches these, statements.cpp), and even
    // there almost always written as "" (unlabeled loops/breaks/continues)
    // -- only a real `label:` statement ever makes one of these non-empty.
    // Once allocated (by any one field going non-empty), stays allocated
    // for the Context's lifetime -- the tree-walker's common restore-to-
    // previous pattern (set_current_loop_label(prev_loop_label) at loop
    // exit) must not thrash alloc/free every iteration; see
    // ensure_loop_labels() and each setter below.
    struct LoopLabelState {
        std::string break_label;
        std::string continue_label;
        std::string current_loop_label;
        std::string next_statement_label;
    };
    std::unique_ptr<LoopLabelState> loop_labels_;
    LoopLabelState& ensure_loop_labels() {
        if (!loop_labels_) loop_labels_ = std::make_unique<LoopLabelState>();
        return *loop_labels_;
    }

    Object* last_super_override_ = nullptr;  // comparison-only, see last_super_override()
    Environment* owned_env_ = nullptr;  // see set_owned_env()
    Value new_target_;

    Engine* engine_;

    // Invariant for a whole script/module's Context tree (only ever set on
    // a root/module Context, see set_current_filename()'s own call sites),
    // so every child previously paid a full std::string copy (often a heap
    // allocation) at construction just to inherit an unchanging value.
    // Interned via Shape::intern() (same pool, same write-only-on-set
    // discipline -- see that method's own doc comment) so inheriting it is
    // now a plain pointer copy.
    const std::string* current_filename_;

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
    // Lazy: null unless a tree-walked (non-VM-compiled) function/generator/
    // async call with >=1 parameter actually sets a non-empty name set --
    // see set_eval_param_names()'s own empty-set guard below. VM-compiled
    // ordinary calls (the majority) never touch this at all.
    std::unique_ptr<std::unordered_set<std::string>> eval_param_names_;
    Value import_meta_;

    // Dispose scope stack for 'using' declarations (Explicit Resource
    // Management) -- lazy, since only Contexts whose AST actually contains
    // a using/await using declaration ever touch this (push_dispose_scope()
    // below allocates it on first use).
    struct DisposableResource {
        Value resource_value;     // passed as 'this' to dispose method
        Value dispose_method;     // looked up once at initialization time
        bool is_async_dispose;    // `await using` (vs `using`): Dispose() must Await() the call's result
    };
    std::unique_ptr<std::vector<std::vector<DisposableResource>>> dispose_scope_stack_;

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

    // Everything reset_for_call() does not write is still in the state a
    // fresh context would have, so reusing this one cannot leak anything from
    // the call that just finished. Checked before pooling, never assumed:
    // any context that fails simply is not pooled.
    bool is_pristine() const {
        return !builtins_ && !loop_labels_ && !eval_param_names_ && !dispose_scope_stack_ &&
               !owned_env_ && microtask_queue_.empty() && draining_queue_.empty();
    }
    // The fields a call can observe, back to what the constructor would have
    // produced. Mirrors Context(Engine*, Context*, Type::Function) exactly;
    // the two must be changed together.
    void reset_for_call(Engine* engine, Context* parent);
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
    const std::unordered_set<std::string>& get_eval_param_names() const {
        static const std::unordered_set<std::string> kEmpty;
        return eval_param_names_ ? *eval_param_names_ : kEmpty;
    }
    void set_eval_param_names(std::unordered_set<std::string> names) {
        // Leaf stays leaf: this is called unconditionally (even for a
        // zero-parameter function) by every tree-walked/generator/async
        // call, so only allocate when there's genuinely something to store.
        if (names.empty()) { eval_param_names_.reset(); return; }
        eval_param_names_ = std::make_unique<std::unordered_set<std::string>>(std::move(names));
    }
    bool is_in_class_field_init() const { return in_class_field_init_; }
    void set_in_class_field_init(bool v) { in_class_field_init_ = v; }
    Value get_import_meta();
    void set_import_meta(const Value& v) { import_meta_ = v; }
    
    const std::string& get_current_filename() const { return *current_filename_; }
    void set_current_filename(const std::string& filename) { current_filename_ = Shape::intern(filename); }
    
    bool is_strict_mode() const { return strict_mode_; }
    void set_strict_mode(bool strict) { strict_mode_ = strict; }
    bool original_this_was_nullish() const { return original_this_was_nullish_; }
    void set_original_this_nullish(bool v) { original_this_was_nullish_ = v; }
    bool original_this_was_primitive() const { return original_this_was_primitive_; }
    void set_original_this_primitive(bool v) { original_this_was_primitive_ = v; }

    Object* get_global_object() const { return global_object_; }
    void set_global_object(Object* global);

    Object* get_this_binding() const {
        return (this_value_.is_object() || this_value_.is_function())
            ? this_value_.as_object() : nullptr;
    }
    void set_this_binding(Object* this_obj) {
        this_value_ = this_obj ? Value(this_obj) : Value();
    }
    // The receiver as it really is, primitive included. `this` is a frame's
    // own state, not a scope-chain binding, so the binding API below routes
    // the name straight here.
    const Value& get_this_value() const { return this_value_; }
    void set_this_value(const Value& v) { this_value_ = v; }

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
    // The realm's %String.prototype% and friends, captured once the builtins
    // are installed. A primitive's property lookup goes through these: the spec
    // reaches for the intrinsic and never for the global String/Number/...
    // binding, which user code can reassign -- and resolving through that
    // binding cost a scope lookup plus a property read on every access.
    enum class PrimitiveKind : uint8_t { String, Number, Boolean, BigInt, Symbol, Count };
    static Object* primitive_prototype(PrimitiveKind kind);
    // The realm's %Promise%, captured the same way and for the same reason:
    // Promise.resolve's fast path has to know it is dealing with the untouched
    // constructor before it may skip building a capability.
    static Function* intrinsic_promise();
    void capture_primitive_prototypes();

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
    const std::string& get_break_label() const {
        static const std::string kEmpty;
        return loop_labels_ ? loop_labels_->break_label : kEmpty;
    }
    const std::string& get_continue_label() const {
        static const std::string kEmpty;
        return loop_labels_ ? loop_labels_->continue_label : kEmpty;
    }
    void set_break(const std::string& label = "");
    void set_continue(const std::string& label = "");
    void clear_break_continue();

    const std::string& get_current_loop_label() const {
        static const std::string kEmpty;
        return loop_labels_ ? loop_labels_->current_loop_label : kEmpty;
    }
    void set_current_loop_label(const std::string& label) {
        if (label.empty() && !loop_labels_) return;
        ensure_loop_labels().current_loop_label = label;
    }

    const std::string& get_next_statement_label() const {
        static const std::string kEmpty;
        return loop_labels_ ? loop_labels_->next_statement_label : kEmpty;
    }
    void set_next_statement_label(const std::string& label) {
        if (label.empty() && !loop_labels_) return;
        ensure_loop_labels().next_statement_label = label;
    }

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

/**
 * Environment for variable bindings
 */
// Counts the events that can put an Environment* somewhere it will outlive the
// scope that made it: a closure capturing one, and either survivor pool taking
// one. An Environment can only be captured by something created during its own
// lifetime -- anything older never saw it, anything newer comes after it is
// gone -- so if this has not moved between an environment's construction and
// its release, nothing can be holding it and it is provably dead.
//
// The direction matters: this is only ever used to take a faster path when it
// proves nothing happened. Missing a bump would free a live environment, so
// every site that stores one beyond its scope has to be here. They are listed
// at bump_capture_epoch's definition.
uint32_t capture_epoch();
void bump_capture_epoch();

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
        // Whether the binding came from a let/const (and which). These are
        // facts about one binding, so they belong on it -- they used to live in
        // a pair of name-keyed hash sets, which meant a block with a let paid a
        // set, its bucket array and a copy of every name, on every entry.
        // Object environments keep no slot, so those still use the sets below.
        bool lexical = false;
        bool const_binding = false;
    };

    // Like HybridDescriptorMap's inline array (Object.h), but can't copy its
    // migrate-to-overflow-when-full step: stable_binding_slot() hands out a
    // raw Value* that BytecodeChunk::lookup_cache and
    // Function::instance_lookup_cache() cache PERMANENTLY (Op::LdaLookup/
    // StaLookup trust it for the owning chunk/Function's whole lifetime), and
    // migration would silently invalidate that pointer. So: inline and
    // overflow entries, once populated, NEVER move. Erasing an inline entry
    // tombstones it in place instead of compacting (compaction would relocate
    // a survivor). Tombstones ARE reused by later inserts -- safe because
    // stable_binding_slot() refuses deletable bindings, the only kind that
    // can ever be erased, so a tombstoned slot never has a live cached
    // pointer. Re-audit lookup_cache/instance_lookup_cache() before relaxing
    // any of this.
    struct SlotMap {
        static constexpr size_t kInlineCapacity = 4;
        // key is interned (Shape::intern(), same pool Shape's own SlotMap/
        // TransitionMap use) instead of a 32-byte embedded std::string --
        // 8 bytes per entry instead. find()/inline_slot() (the get/set hot
        // path) compare against the dereferenced pointee BY VALUE and never
        // intern; only get_or_create()'s insert branch below calls
        // Shape::intern(), once per binding-creation event.
        struct InlineEntry {
            const std::string* key = nullptr;
            BindingSlot slot;
            bool in_use = false;
        };
        std::array<InlineEntry, kInlineCapacity> inline_entries;
        // Stays keyed by plain std::string (the rare/slow spill path once
        // inline capacity is exceeded) -- it's a unique_ptr either way, so
        // interning it wouldn't shrink Environment, same rationale as
        // Shape::SlotMap's own overflow map.
        using OverflowMap = std::unordered_map<std::string, BindingSlot, std::hash<std::string>,
                                                std::equal_to<std::string>,
                                                SmallMapAllocator<std::pair<const std::string, BindingSlot>>>;
        std::unique_ptr<OverflowMap> overflow;

        BindingSlot* find(const std::string& name) {
            for (auto& e : inline_entries) {
                if (e.in_use && *e.key == name) return &e.slot;
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
                    e.key = Shape::intern(name);
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
                if (e.in_use && *e.key == name) {
                    e.in_use = false;
                    e.slot = BindingSlot{};
                    e.key = nullptr;
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
                if (e.in_use) fn(*e.key, e.slot);
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
    // Lazy, bundled: only the specific Environment hosting a let/const/
    // named-class declaration at its own top level ever populates either
    // set (mark_lexical_declaration()/mark_const_binding() below) -- empty
    // for every var-only function env, plain block/loop/if body with no
    // top-level lexical declaration, with-environment, and catch-clause
    // environment.
    struct LexicalNames {
        std::unordered_set<std::string> lexical;
        std::unordered_set<std::string> const_binding; // tracks const declarations in Object envs
    };
    std::unique_ptr<LexicalNames> lexical_names_;
    Object* binding_object_;
    bool is_with_environment_ = false; // ES6 8.1.1.2.1 HasBinding: only `with` object environments consult @@unscopables
    bool is_closure_boundary_ = false; // marks script-level env: stop snapshot loops here
    bool escaped_ = false;  // see is_escaped()
    // capture_epoch() as of construction -- see its comment above.
    uint32_t birth_epoch_ = 0;

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
    // has_own_binding followed by is_initialized_binding asks slots_ for the
    // same name twice, and the TDZ check on every uncached name read did
    // exactly that. Declarative environments only: for them has_own_binding
    // IS this lookup, so folding the two changes nothing but the count.
    // Returns false when the environment does not own `name`; the caller must
    // then keep walking outward, since the answer belongs to the NEAREST
    // owner and nothing further in.
    bool declarative_binding_tdz(const std::string& name, bool& in_tdz) const {
        if (type_ == Type::Object) return false;
        const BindingSlot* slot = slots_.find(name);
        if (!slot) return false;
        in_tdz = !slot->initialized;
        return true;
    }
    void set_with_environment(bool value) { is_with_environment_ = value; }
    bool is_closure_boundary() const { return is_closure_boundary_; }
    void mark_closure_boundary() { is_closure_boundary_ = true; }

    // Escape tracking: a block-scope env that was never captured (by a closure's
    // closure_environment_, a child context's outer chain, or an eval env) can be
    // deleted on pop instead of leaking. Marking walks the outer chain so every
    // env reachable from a captured one is pinned too.
    bool is_escaped() const { return escaped_; }
    // True when nothing that could have captured this environment was created
    // during its lifetime, which makes it unreachable without asking the
    // collector.
    bool provably_unreachable() const {
        const uint32_t now = capture_epoch();
        return now != UINT32_MAX && birth_epoch_ == now;
    }
    void mark_escaped() {
        // Something is taking a reference that outlives this scope, which is
        // exactly what the capture epoch counts.
        bump_capture_epoch();
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
    // Address-stable slot for the lookup cache. Immutable bindings qualify --
    // their address is as stable as any other and their value never moves --
    // so `writable` reports back whether a store may go through the pointer.
    Value* stable_binding_slot(const std::string& name, bool* writable = nullptr);
    // The object-environment counterpart: the shape slot index a plain own data
    // property of this environment's binding object lives at, for LdaLookup's
    // cache. False for anything the general path has to serve.
    bool cacheable_object_binding(const std::string& name, uint32_t& slot_index) const;
    // Guarded direct-index access to slots_'s inline array, backing
    // Op::LdaEnvSlot/StaEnvSlot/StaEnvSlotInit. The compiler's predicted
    // index can be wrong (see BytecodeCompiler.h's EnvSlotInfo for why), so
    // this re-validates by name: returns null unless inline_entries[index]
    // is in_use AND its key equals `name`. A null return means "fall back
    // to the name-based path," never "this binding doesn't exist."
    SlotMap::InlineEntry* inline_slot(size_t index, const std::string& name) {
        if (index >= SlotMap::kInlineCapacity) return nullptr;
        SlotMap::InlineEntry& e = slots_.inline_entries[index];
        if (e.in_use && *e.key == name) return &e;
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
    // has_own_binding + get_binding_direct in one pass. A chain walk that then
    // re-reads the winning environment asks an object environment the same
    // question up to three times (HasBinding, GetBindingValue's own re-check,
    // then the Get), each a hash on the global object. Returns false when this
    // environment does not bind `name` at all, leaving `out` untouched.
    bool try_get_binding(const std::string& name, Value& out, Context* ctx) const;
    bool has_lexical_declaration(const std::string& name) const {
        if (const BindingSlot* s = slots_.find(name)) return s->lexical;
        return lexical_names_ && lexical_names_->lexical.count(name) > 0;
    }
    void mark_lexical_declaration(const std::string& name) {
        if (BindingSlot* s = slots_.find(name)) { s->lexical = true; return; }
        if (!lexical_names_) lexical_names_ = std::make_unique<LexicalNames>();
        lexical_names_->lexical.insert(name);
    }
    bool is_const_binding(const std::string& name) const {
        if (const BindingSlot* s = slots_.find(name)) return s->const_binding;
        return lexical_names_ && lexical_names_->const_binding.count(name) > 0;
    }
    void mark_const_binding(const std::string& name) {
        if (BindingSlot* s = slots_.find(name)) { s->const_binding = true; return; }
        if (!lexical_names_) lexical_names_ = std::make_unique<LexicalNames>();
        lexical_names_->const_binding.insert(name);
    }
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
// A function call's context is identical from one call to the next except for
// a handful of fields, so a returning call hands its context back to be used
// again rather than destroying it and building a new one over the same bytes.
// Only a context that is provably done can be reused -- see is_pristine() --
// and only one that provably cannot be reached from anywhere else, which is
// the exact condition ContextSurvivorGuard already decides.
class CallContextPool {
public:
    // Constructed and reset, ready for a call whose parent is `parent`.
    static Context* acquire(Engine* engine, Context* parent);
    // Takes the context back if it can, destroys or hands it to the engine's
    // survivor pool if it cannot.
    static void release(Context* ctx, Engine* engine);
    static void drain();   // engine teardown
};

struct ContextSurvivorGuard {
    std::unique_ptr<Context>& ptr;
    Engine* eng;
    ContextSurvivorGuard(std::unique_ptr<Context>& p, Engine* e) : ptr(p), eng(e) {}
    // Defined in Context.cpp: Engine is only forward-declared here.
    ~ContextSurvivorGuard();
};

}

#endif
