/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The WebAssembly namespace of JavaScript: Module, Instance, Memory, Table, Global and the rest, and the conversions between the values of the
// two languages.

#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "quanta/Embed.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/wasm/Decoder.h"
#include "quanta/wasm/Runtime.h"

namespace Quanta::Wasm {

namespace {

namespace E = Quanta::Embed;

using Args = E::Args;

struct RealmData {
    Object* moduleProto = nullptr;
    Object* instanceProto = nullptr;
    Object* memoryProto = nullptr;
    Object* tableProto = nullptr;
    Object* globalProto = nullptr;
    Object* tagProto = nullptr;
    Object* exceptionProto = nullptr;
    TagObject* jsTag = nullptr;
    Value compileError, linkError, runtimeError;
};

const char kRealmKey = 0;
const char kModuleKey = 0, kInstanceKey = 0, kMemoryKey = 0, kTableKey = 0, kGlobalKey = 0, kTagKey = 0, kExceptionKey = 0;

RealmData& R(Context& ctx) { return *static_cast<RealmData*>(E::GetRealmData(ctx, &kRealmKey)); }

// ---- Errors ----

void ThrowKind(Context& ctx, const Value& ctor, const std::string& message) {
    const Value text = E::NewString(ctx, message);
    const Value error = E::Construct(ctx, ctor, Args(&text, 1));
    if (E::HasException(ctx)) return;
    E::Throw(ctx, error);
}
void ThrowCompile(Context& ctx, const std::string& message) { ThrowKind(ctx, R(ctx).compileError, message); }
void ThrowLink(Context& ctx, const std::string& message) { ThrowKind(ctx, R(ctx).linkError, message); }
void ThrowRuntime(Context& ctx, const std::string& message) { ThrowKind(ctx, R(ctx).runtimeError, message); }

Value Undef() { return Value(); }
bool IsObjectLike(const Value& v) { return v.is_object() || v.is_function(); }

// ---- Numbers ----

// ToBigInt64: the 64 bits of the BigInt a value comes to (modulo 2^64).
bool ToBigInt(Context& ctx, const Value& v, Value& out) {
    if (v.is_bigint()) { out = v; return true; }
    if (v.is_boolean()) { out = Value(new BigInt(v.as_boolean() ? 1 : 0)); return true; }
    if (v.is_string()) {
        try {
            out = Value(new BigInt(v.as_string()->str()));
            return true;
        } catch (...) {
            ctx.throw_syntax_error("Cannot convert string to a BigInt");
            return false;
        }
    }
    if (v.is_object() || v.is_function()) {
        Object* obj = v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object();
        Symbol* toPrimitive = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
        Value method = toPrimitive ? obj->get_property(toPrimitive->to_property_key()) : Value();
        if (ctx.has_exception()) return false;
        if (!method.is_undefined() && !method.is_null()) {
            if (!method.is_function()) { ctx.throw_type_error("Symbol.toPrimitive is not a function"); return false; }
            const Value hint = Value(std::string("number"));
            Value r = method.as_function()->call(ctx, {hint}, v);
            if (ctx.has_exception()) return false;
            if (r.is_object() || r.is_function()) { ctx.throw_type_error("Cannot convert object to a BigInt"); return false; }
            return ToBigInt(ctx, r, out);
        }
        for (const char* name : {"valueOf", "toString"}) {
            Value fn = obj->get_property(name);
            if (ctx.has_exception()) return false;
            if (!fn.is_function()) continue;
            Value r = fn.as_function()->call(ctx, {}, v);
            if (ctx.has_exception()) return false;
            if (!r.is_object() && !r.is_function()) return ToBigInt(ctx, r, out);
        }
    }
    ctx.throw_type_error("Cannot convert value to a BigInt");
    return false;
}

bool ToInt64Bits(Context& ctx, const Value& v, uint64_t& out) {
    Value big;
    if (!ToBigInt(ctx, v, big)) return false;
    const BigInt& b = *big.as_bigint();
    static const BigInt mask = BigInt::from_string("18446744073709551615");
    static const BigInt low = BigInt::from_string("4294967295");
    const BigInt r = b.bitwise_and(mask);
    const uint64_t hi = static_cast<uint64_t>(r.right_shift(BigInt(32)).bitwise_and(low).to_int64());
    const uint64_t lo = static_cast<uint64_t>(r.bitwise_and(low).to_int64());
    out = (hi << 32) | lo;
    return true;
}

Value FromInt64Bits(uint64_t v) { return Value(new BigInt(static_cast<int64_t>(v))); }

bool ToNumberChecked(Context& ctx, const Value& v, double& out) {
    out = E::ToNumber(ctx, v);
    return !E::HasException(ctx);
}

// The values an iterable gives.
bool IterateToVector(Context& ctx, const Value& iterable, std::vector<Value>& out) {
    if (!IsObjectLike(iterable)) { ctx.throw_type_error("value must be iterable"); return false; }
    Symbol* symbol = Symbol::get_well_known(Symbol::ITERATOR);
    const Value method = E::Get(ctx, iterable, Value(symbol));
    if (E::HasException(ctx)) return false;
    if (!method.is_function()) { ctx.throw_type_error("value must be iterable"); return false; }
    const Value iterator = E::Call(ctx, method, iterable);
    if (E::HasException(ctx)) return false;
    if (!IsObjectLike(iterator)) { ctx.throw_type_error("iterator must be an object"); return false; }
    const Value next = E::Get(ctx, iterator, "next");
    if (E::HasException(ctx)) return false;
    for (;;) {
        const Value result = E::Call(ctx, next, iterator);
        if (E::HasException(ctx)) return false;
        if (!IsObjectLike(result)) { ctx.throw_type_error("iterator result must be an object"); return false; }
        const Value done = E::Get(ctx, result, "done");
        if (E::HasException(ctx)) return false;
        if (E::ToBoolean(done)) return true;
        out.push_back(E::Get(ctx, result, "value"));
        if (E::HasException(ctx)) return false;
        if (out.size() > 1000000) { ctx.throw_range_error("too many values"); return false; }
    }
}

// ---- Functions ----

std::unordered_map<const Object*, FuncObject*>& FunctionMap() {
    static auto* map = new std::unordered_map<const Object*, FuncObject*>();  // (never destroyed: functions may be swept at exit)
    return *map;
}

struct MapGuard {
    const Object* function = nullptr;
    ~MapGuard() { if (function) FunctionMap().erase(function); }
};

Value CallExport(Context& ctx, FuncObject* f, Args args);

// The JavaScript function of a function of a module.
Value WrapperOf(Context& ctx, FuncObject* f) {
    if (Object* existing = f->wrapper.Get()) return Value(existing);
    const CanonType& sig = TypeRegistry::Get(f->sig);
    auto held = std::make_shared<E::Persistent>(ctx, Value(static_cast<Object*>(f)));
    // (The map forgets the function when it is swept: its closure, which owns the guard, goes with it.)
    auto guard = std::make_shared<MapGuard>();
    Value fn = E::NewFunction(ctx, std::to_string(f->index), static_cast<int>(sig.func.params.size()), [held, guard](Context& c, Value, Args a, Value) -> Value {
        FuncObject* target = DOMObject::Cast<FuncObject>(held->Get());
        return CallExport(c, target, a);
    });
    Object* object = fn.is_function() ? static_cast<Object*>(fn.as_function()) : fn.as_object();
    FunctionMap()[object] = f;
    guard->function = object;
    f->wrapper = WeakHandle<Object>(object);
    return fn;
}

FuncObject* FunctionOf(const Value& v) {
    if (!v.is_function()) return nullptr;
    const auto it = FunctionMap().find(static_cast<Object*>(v.as_function()));
    return it == FunctionMap().end() ? nullptr : it->second;
}

// ---- Values ----

bool IsFuncHeap(HeapType h) { return h == Heap::Func || h == Heap::NoFunc || (h >= 0 && TypeRegistry::Get(static_cast<uint32_t>(h)).kind == TypeDef::Kind::Func); }

// ToWebAssemblyValue: the slots of a value of the type.
bool ToWasm(Context& ctx, const Value& v, const ValType& t, uint64_t* n, Value* r) {
    switch (t.kind) {
        case ValType::Kind::I32: {
            double d;
            if (!ToNumberChecked(ctx, v, d)) return false;
            *n = static_cast<uint32_t>(js_to_int32(d));
            return true;
        }
        case ValType::Kind::I64: return ToInt64Bits(ctx, v, *n);
        case ValType::Kind::F32: {
            double d;
            if (!ToNumberChecked(ctx, v, d)) return false;
            const float f = static_cast<float>(d);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            *n = bits;
            return true;
        }
        case ValType::Kind::F64: {
            double d;
            if (!ToNumberChecked(ctx, v, d)) return false;
            std::memcpy(n, &d, 8);
            return true;
        }
        case ValType::Kind::V128: ctx.throw_type_error("type v128 is not allowed in the JavaScript API"); return false;
        case ValType::Kind::Ref: {
            if (IsFuncHeap(t.heap)) {
                if (v.is_null()) {
                    if (!t.nullable) { ctx.throw_type_error("function-typed object expected"); return false; }
                    *r = Value::null();
                    return true;
                }
                FuncObject* f = FunctionOf(v);
                if (!f) { ctx.throw_type_error("function-typed object expected"); return false; }
                if (t.heap != Heap::Func && !TypeRegistry::HeapSubtype(static_cast<HeapType>(f->sig), t.heap)) { ctx.throw_type_error("function-typed object has the wrong type"); return false; }
                *r = Value(static_cast<Object*>(f));
                return true;
            }
            if (t.heap == Heap::NoExtern) {
                if (v.is_null() && t.nullable) { *r = Value::null(); return true; }
                ctx.throw_type_error("invalid value for a reference type");
                return false;
            }
            if (t.heap == Heap::Extern) {
                if (v.is_null()) {
                    if (!t.nullable) { ctx.throw_type_error("externref must not be null"); return false; }
                    *r = Value::null();
                    return true;
                }
                *r = v;
                return true;
            }
            if (v.is_null()) {
                if (!t.nullable) { ctx.throw_type_error("invalid value for a reference type"); return false; }
                *r = Value::null();
                return true;
            }
            if (TypeRegistry::TopOf(t.heap) == Heap::Exn) {
                if (!DOMObject::Cast<ExceptionObject>(v)) { ctx.throw_type_error("exnref expected"); return false; }
                *r = v;
                return true;
            }
            // (The any hierarchy: a number of an i31, a struct or an array of a module, or whatever else, as a host reference.)
            Value ref = v;
            const bool i31 = v.is_number() && v.as_number() == std::trunc(v.as_number()) && v.as_number() >= -1073741824.0 && v.as_number() <= 1073741823.0 && !(v.as_number() == 0 && std::signbit(v.as_number()));
            if (!i31 && !DOMObject::Cast<StructObject>(v) && !DOMObject::Cast<ArrayObject>(v)) {
                HostRefObject* h = ::Quanta::Heap::Allocate<HostRefObject>();
                h->initialize_prototype(nullptr);
                h->prevent_extensions();
                h->value.Set(v);
                ref = Value(static_cast<Object*>(h));
            }
            if (!RefMatches(ref, t.heap, t.nullable)) { ctx.throw_type_error("invalid value for a reference type"); return false; }
            *r = ref;
            return true;
        }
        default: return false;
    }
}

Value ToJS(Context& ctx, const ValType& t, const uint64_t* n, const Value* r) {
    switch (t.kind) {
        case ValType::Kind::I32: return Value(static_cast<int32_t>(static_cast<uint32_t>(*n)));
        case ValType::Kind::I64: return FromInt64Bits(*n);
        case ValType::Kind::F32: {
            float f;
            const uint32_t bits = static_cast<uint32_t>(*n);
            std::memcpy(&f, &bits, 4);
            return Value(static_cast<double>(f));
        }
        case ValType::Kind::F64: {
            double d;
            std::memcpy(&d, n, 8);
            return Value(d);
        }
        case ValType::Kind::Ref: {
            if (r->is_null()) return Value::null();
            if (IsFuncHeap(t.heap)) {
                if (FuncObject* f = DOMObject::Cast<FuncObject>(*r)) return WrapperOf(ctx, f);
            }
            if (HostRefObject* h = DOMObject::Cast<HostRefObject>(*r)) return h->value.Get();
            return *r;
        }
        default: return Value();
    }
}

struct Slots {
    uint32_t n = 0, r = 0;
};
Slots CountSlots(const std::vector<ValType>& types) {
    Slots s;
    for (const ValType& t : types) {
        if (t.IsRef()) ++s.r;
        else s.n += t.kind == ValType::Kind::V128 ? 2 : 1;
    }
    return s;
}

void ThrowTrap(Context& ctx, TrapKind kind) {
    if (kind == TrapKind::StackOverflow) {
        ctx.throw_range_error("Maximum call stack size exceeded");
        return;
    }
    ThrowRuntime(ctx, TrapMessage(kind));
}

Value CallExport(Context& ctx, FuncObject* f, Args args) {
    if (!f) { ctx.throw_type_error("Illegal invocation"); return Value(); }
    const CanonType& sig = TypeRegistry::Get(f->sig);
    const Slots params = CountSlots(sig.func.params);
    const Slots results = CountSlots(sig.func.results);
    std::vector<uint64_t> n(std::max(params.n, results.n) + 1);
    std::vector<Value> r(std::max(params.r, results.r) + 1, Value::null());
    uint32_t ni = 0, ri = 0;
    for (size_t i = 0; i < sig.func.params.size(); ++i) {
        const Value arg = i < args.size() ? args[i] : Value();
        if (!ToWasm(ctx, arg, sig.func.params[i], &n[ni], &r[ri])) return Value();
        if (sig.func.params[i].IsRef()) ++ri;
        else ni += sig.func.params[i].kind == ValType::Kind::V128 ? 2 : 1;
    }
    Machine& machine = Machine::Current();
    const Machine::Status status = machine.Invoke(ctx, f, n.data(), r.data());
    if (status == Machine::Status::Trap) {
        ThrowTrap(ctx, machine.lastTrap);
        return Value();
    }
    if (status == Machine::Status::Exception) return Value();
    if (sig.func.results.empty()) return Value();
    std::vector<Value> out;
    ni = 0;
    ri = 0;
    for (const ValType& t : sig.func.results) {
        out.push_back(ToJS(ctx, t, &n[ni], &r[ri]));
        if (t.IsRef()) ++ri;
        else ni += t.kind == ValType::Kind::V128 ? 2 : 1;
    }
    if (out.size() == 1) return out[0];
    return E::NewArray(ctx, Args(out.data(), out.size()));
}

}  // namespace

// A function of the host, called from WebAssembly: its parameters are slots, and its results are written over them.
Machine::Status Machine::CallHost(Context& ctx, FuncObject* f, uint64_t* n, Value* r) {
    const CanonType& sig = TypeRegistry::Get(f->sig);
    std::vector<Value> args;
    uint32_t ni = 0, ri = 0;
    for (const ValType& t : sig.func.params) {
        args.push_back(ToJS(ctx, t, &n[ni], &r[ri]));
        if (t.IsRef()) ++ri;
        else ni += t.kind == ValType::Kind::V128 ? 2 : 1;
    }
    const Value result = E::Call(ctx, f->host.Get(), Value(), Args(args.data(), args.size()));
    if (E::HasException(ctx)) return Status::Exception;
    const std::vector<ValType>& results = sig.func.results;
    if (results.empty()) return Status::Ok;
    std::vector<Value> values;
    if (results.size() == 1) {
        values.push_back(result);
    } else {
        // (More than one: the result is read as an iterable of that many.)
        if (!IterateToVector(ctx, result, values)) return Status::Exception;
        if (values.size() != results.size()) { ctx.throw_type_error("multi-return length mismatch"); return Status::Exception; }
    }
    ni = 0;
    ri = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (!ToWasm(ctx, values[i], results[i], &n[ni], &r[ri])) return Status::Exception;
        if (results[i].IsRef()) ++ri;
        else ni += results[i].kind == ValType::Kind::V128 ? 2 : 1;
    }
    return Status::Ok;
}

namespace {

// ---- Objects of the classes ----

template <class T>
T* NewObjectOf(Object* proto) {
    T* o = ::Quanta::Heap::Allocate<T>();
    o->initialize_prototype(proto);
    return o;
}

MemoryObject* NewMemory(Context& ctx, const MemType& type) {
    MemoryObject* m = NewObjectOf<MemoryObject>(R(ctx).memoryProto);
    if (!m->Init(type)) return nullptr;
    return m;
}

}  // namespace

void NotifyMemoryGrown(Context& ctx, MemoryObject* memory) {
    const Value old = memory->buffer.Get();
    if (old.is_object()) {
        E::DetachArrayBuffer(ctx, old);
        memory->buffer.Clear();
    }
}

ExceptionObject* NewException(Context& ctx, TagObject* tag) {
    ExceptionObject* e = ::Quanta::Heap::Allocate<ExceptionObject>();
    e->initialize_prototype(R(ctx).exceptionProto);
    e->tag.Set(Value(static_cast<Object*>(tag)));
    return e;
}

ExceptionObject* WrapException(Context& ctx, const Value& thrown) {
    if (ExceptionObject* e = DOMObject::Cast<ExceptionObject>(thrown)) return e;
    ExceptionObject* e = NewException(ctx, R(ctx).jsTag);
    e->refs.push_back(thrown);
    return e;
}

void ThrowException(Context& ctx, ExceptionObject* e) {
    if (DOMObject::Cast<TagObject>(e->tag.Get()) == R(ctx).jsTag && e->refs.size() == 1) E::Throw(ctx, e->refs[0]);
    else E::Throw(ctx, Value(static_cast<Object*>(e)));
}

namespace {

// ---- Compiling ----

bool Compile(Context& ctx, const Value& source, const char* who, std::shared_ptr<Module>& out) {
    const std::optional<std::span<const uint8_t>> bytes = E::BytesOf(source);
    if (!bytes) {
        ctx.throw_type_error(std::string(who) + ": Argument 0 must be a buffer source");
        return false;
    }
    auto module = std::make_shared<Module>();
    std::string error;
    if (!Decode(bytes->data(), bytes->size(), *module, error) || !Validate(*module, error)) {
        ThrowCompile(ctx, std::string(who) + ": " + error);
        return false;
    }
    out = std::move(module);
    return true;
}

ModuleObject* ModuleOf(Context& ctx, const Value& v, const char* who) {
    ModuleObject* m = DOMObject::Cast<ModuleObject>(v);
    if (!m) ctx.throw_type_error(std::string(who) + ": Argument 0 must be a WebAssembly.Module");
    return m;
}

const char* KindName(ExternKind k) {
    switch (k) {
        case ExternKind::Func: return "function";
        case ExternKind::Table: return "table";
        case ExternKind::Memory: return "memory";
        case ExternKind::Global: return "global";
        case ExternKind::Tag: return "tag";
    }
    return "";
}

// ---- Instantiating ----

bool EvalConst(Context& ctx, InstanceObject* instance, const ByteRange& range, const ValType& type, uint64_t& lo, uint64_t& hi, Value& ref) {
    uint64_t n[4] = {0, 0, 0, 0};
    Value r[1] = {Value::null()};
    const Machine::Status status = Machine::Current().EvaluateConst(ctx, instance, instance->module->constExprs[static_cast<size_t>(range.compiled)], n, r);
    if (status != Machine::Status::Ok) {
        if (status == Machine::Status::Trap) ThrowTrap(ctx, Machine::Current().lastTrap);
        return false;
    }
    lo = n[0];
    hi = n[1];
    ref = r[0];
    (void)type;
    return true;
}

bool LimitsMatch(const Limits& have, uint64_t haveSize, uint64_t haveMax, bool haveHasMax, const Limits& want) {
    if (have.is64 != want.is64) return false;
    if (haveSize < want.min) return false;
    if (want.hasMax) {
        if (!haveHasMax || haveMax > want.max) return false;
    }
    return true;
}

InstanceObject* Instantiate(Context& ctx, const std::shared_ptr<Module>& module, const Value& imports, const char* who) {
    const Module& m = *module;
    if (!m.imports.empty() && !imports.is_object() && !imports.is_function()) {
        ctx.throw_type_error(std::string(who) + ": Argument 1 must be an object");
        return nullptr;
    }
    InstanceObject* inst = NewObjectOf<InstanceObject>(R(ctx).instanceProto);
    inst->module = module;
    inst->funcs.assign(m.funcTypes.size(), nullptr);
    // The imports.
    uint32_t fi = 0, ti = 0, mi = 0, gi = 0, tgi = 0;
    inst->tables.assign(m.tableTypes.size(), nullptr);
    inst->memories.assign(m.memTypes.size(), nullptr);
    inst->globals.assign(m.globalTypes.size(), nullptr);
    inst->tags.assign(m.tagTypes.size(), nullptr);
    for (size_t k = 0; k < m.imports.size(); ++k) {
        const Import& im = m.imports[k];
        const std::string where = std::string(who) + ": Import #" + std::to_string(k) + " \"" + im.module + "\"";
        const Value ns = E::Get(ctx, imports, im.module);
        if (E::HasException(ctx)) return nullptr;
        if (!ns.is_object() && !ns.is_function()) {
            ctx.throw_type_error(where + ": module is not an object or function");
            return nullptr;
        }
        const Value v = E::Get(ctx, ns, im.name);
        if (E::HasException(ctx)) return nullptr;
        const std::string at = where + " \"" + im.name + "\"";
        switch (im.kind) {
            case ExternKind::Func: {
                if (!v.is_function()) { ThrowLink(ctx, at + ": function import requires a callable"); return nullptr; }
                const uint32_t sig = m.funcSigs[fi];
                if (FuncObject* existing = FunctionOf(v)) {
                    if (existing->sig != sig && !TypeRegistry::HeapSubtype(static_cast<HeapType>(existing->sig), static_cast<HeapType>(sig))) {
                        ThrowLink(ctx, at + ": imported function does not match the expected type");
                        return nullptr;
                    }
                    inst->funcs[fi++] = existing;
                } else {
                    FuncObject* f = NewObjectOf<FuncObject>(nullptr);
                    f->sig = sig;
                    f->index = fi;
                    f->host.Set(v);
                    inst->funcs[fi++] = f;
                }
                break;
            }
            case ExternKind::Table: {
                TableObject* t = DOMObject::Cast<TableObject>(v);
                if (!t) { ThrowLink(ctx, at + ": table import requires a WebAssembly.Table"); return nullptr; }
                const TableType want{ctx.realm() ? TypeRegistry::Map(im.table.elem, m.typeIds) : im.table.elem, im.table.limits};
                if (!LimitsMatch(t->type.limits, t->Size(), t->type.limits.max, t->type.limits.hasMax, want.limits) || t->type.elem != want.elem) {
                    ThrowLink(ctx, at + ": table import has an incompatible type");
                    return nullptr;
                }
                inst->tables[ti++] = t;
                break;
            }
            case ExternKind::Memory: {
                MemoryObject* mem = DOMObject::Cast<MemoryObject>(v);
                if (!mem) { ThrowLink(ctx, at + ": memory import must be a WebAssembly.Memory object"); return nullptr; }
                if (!LimitsMatch(mem->type.limits, mem->pages, mem->type.limits.max, mem->type.limits.hasMax, im.memory.limits)) {
                    ThrowLink(ctx, at + ": memory import has an incompatible type");
                    return nullptr;
                }
                inst->memories[mi++] = mem;
                break;
            }
            case ExternKind::Global: {
                const GlobalType want{TypeRegistry::Map(im.global.type, m.typeIds), im.global.mut};
                if (GlobalObject* g = DOMObject::Cast<GlobalObject>(v)) {
                    const bool typeOk = want.mut ? g->type.type == want.type : TypeRegistry::Matches(g->type.type, want.type);
                    if (g->type.mut != want.mut || !typeOk) { ThrowLink(ctx, at + ": imported global does not match the expected type"); return nullptr; }
                    inst->globals[gi++] = g;
                } else {
                    if (want.mut) { ThrowLink(ctx, at + ": imported mutable global must be a WebAssembly.Global object"); return nullptr; }
                    GlobalObject* fresh = NewObjectOf<GlobalObject>(R(ctx).globalProto);
                    fresh->type = want;
                    if (want.type.kind == ValType::Kind::I64) {
                        if (!v.is_bigint()) { ThrowLink(ctx, at + ": global import must be a BigInt"); return nullptr; }
                    } else if (want.type.IsNum()) {
                        if (!v.is_number()) { ThrowLink(ctx, at + ": global import must be a number"); return nullptr; }
                    } else if (want.type.IsRef()) {
                        // (any value, for a reference type that takes it)
                    } else {
                        ThrowLink(ctx, at + ": global import must be a WebAssembly.Global object");
                        return nullptr;
                    }
                    Value ref;
                    uint64_t lo = 0;
                    Value held;
                    if (!ToWasm(ctx, v, want.type, &lo, &held)) {
                        if (E::HasException(ctx)) return nullptr;
                    }
                    fresh->lo = lo;
                    if (want.type.IsRef()) fresh->ref.Set(held);
                    inst->globals[gi++] = fresh;
                }
                break;
            }
            case ExternKind::Tag: {
                TagObject* t = DOMObject::Cast<TagObject>(v);
                if (!t) { ThrowLink(ctx, at + ": tag import requires a WebAssembly.Tag"); return nullptr; }
                if (t->sig != m.tagSigs[tgi]) { ThrowLink(ctx, at + ": imported tag does not match the expected type"); return nullptr; }
                inst->tags[tgi++] = t;
                break;
            }
        }
    }
    // The functions, tables, memories, globals and tags of the module itself.
    for (uint32_t i = 0; fi + i < m.funcTypes.size(); ++i) {
        FuncObject* f = NewObjectOf<FuncObject>(nullptr);
        f->sig = m.funcSigs[fi + i];
        f->index = fi + i;
        f->instance = inst;
        f->code = &m.compiled[i];
        f->instanceValue.Set(Value(static_cast<Object*>(inst)));
        inst->funcs[fi + i] = f;
    }
    for (size_t i = 0; i < m.tables.size(); ++i) {
        TableObject* t = NewObjectOf<TableObject>(R(ctx).tableProto);
        const TableType type{TypeRegistry::Map(m.tables[i].type.elem, m.typeIds), m.tables[i].type.limits};
        Value init = Value::null();
        inst->tables[ti + i] = t;  // (in place, for what the initial value may read)
        if (m.tables[i].hasInit) {
            uint64_t lo, hi;
            if (!EvalConst(ctx, inst, m.tables[i].init, type.elem, lo, hi, init)) return nullptr;
        }
        if (!t->Init(type, init)) { ctx.throw_range_error(std::string(who) + ": could not allocate a table"); return nullptr; }
    }
    for (size_t i = 0; i < m.memTypes.size() - mi; ++i) {
        MemoryObject* mem = NewMemory(ctx, m.memTypes[mi + i]);
        if (!mem) { ctx.throw_range_error(std::string(who) + ": could not allocate memory"); return nullptr; }
        inst->memories[mi + i] = mem;
    }
    for (size_t i = 0; i < m.tagTypes.size() - tgi; ++i) {
        TagObject* t = NewObjectOf<TagObject>(R(ctx).tagProto);
        t->sig = m.tagSigs[tgi + i];
        inst->tags[tgi + i] = t;
    }
    for (size_t i = 0; i < m.globals.size(); ++i) {
        GlobalObject* g = NewObjectOf<GlobalObject>(R(ctx).globalProto);
        g->type = GlobalType{TypeRegistry::Map(m.globals[i].type.type, m.typeIds), m.globals[i].type.mut};
        inst->globals[gi + i] = g;
        Value ref = Value::null();
        uint64_t lo = 0, hi = 0;
        if (!EvalConst(ctx, inst, m.globals[i].init, g->type.type, lo, hi, ref)) return nullptr;
        g->lo = lo;
        g->hi = hi;
        if (g->type.type.IsRef()) g->ref.Set(ref);
    }
    // The exports.
    {
        Value exports = E::NewObject(ctx);
        E::SetPrototypeOf(ctx, exports, E::Null());
        for (const Export& e : m.exports) {
            Value v;
            switch (e.kind) {
                case ExternKind::Func: v = WrapperOf(ctx, inst->funcs[e.index]); break;
                case ExternKind::Table: v = Value(static_cast<Object*>(inst->tables[e.index])); break;
                case ExternKind::Memory: v = Value(static_cast<Object*>(inst->memories[e.index])); break;
                case ExternKind::Global: v = Value(static_cast<Object*>(inst->globals[e.index])); break;
                case ExternKind::Tag: v = Value(static_cast<Object*>(inst->tags[e.index])); break;
            }
            E::Descriptor d;
            d.value = v; d.has_value = true;
            d.has_writable = true; d.writable = false;
            d.has_enumerable = true; d.enumerable = true;
            d.has_configurable = true; d.configurable = false;
            E::DefineProperty(ctx, exports, e.name, d);
        }
        E::PreventExtensions(ctx, exports);
        inst->exports.Set(exports);
    }
    // The segments.
    inst->elems.resize(m.elems.size());
    for (size_t i = 0; i < m.elems.size(); ++i) {
        const ElemSegment& e = m.elems[i];
        std::vector<Value>& items = inst->elems[i];
        if (e.isFuncIndex) {
            for (uint32_t f : e.funcs) items.push_back(Value(static_cast<Object*>(inst->funcs[f])));
        } else {
            for (const ByteRange& x : e.exprs) {
                uint64_t lo, hi;
                Value ref;
                if (!EvalConst(ctx, inst, x, e.type, lo, hi, ref)) return nullptr;
                items.push_back(ref);
            }
        }
    }
    inst->dataDropped.assign(m.datas.size(), false);
    for (size_t i = 0; i < m.elems.size(); ++i) {
        const ElemSegment& e = m.elems[i];
        if (e.mode == ElemSegment::Mode::Active) {
            uint64_t offset, hi;
            Value ref;
            TableObject* t = inst->tables[e.table];
            if (!EvalConst(ctx, inst, e.offset, ValType::I32(), offset, hi, ref)) return nullptr;
            if (!t->type.limits.is64) offset = static_cast<uint32_t>(offset);
            std::vector<Value>& items = inst->elems[i];
            if (offset > t->Size() || items.size() > t->Size() - offset) {
                ThrowRuntime(ctx, std::string(who) + ": table initializer is out of bounds");
                return nullptr;
            }
            for (size_t k = 0; k < items.size(); ++k) t->elems.set(static_cast<size_t>(offset + k), items[k]);
        }
        if (e.mode != ElemSegment::Mode::Passive) std::vector<Value>().swap(inst->elems[i]);
    }
    for (size_t i = 0; i < m.datas.size(); ++i) {
        const DataSegment& d = m.datas[i];
        if (!d.active) continue;
        uint64_t offset, hi;
        Value ref;
        MemoryObject* mem = inst->memories[d.memory];
        if (!EvalConst(ctx, inst, d.offset, ValType::I32(), offset, hi, ref)) return nullptr;
        if (!mem->type.limits.is64) offset = static_cast<uint32_t>(offset);
        if (offset > mem->Bytes() || d.bytes.size > mem->Bytes() - offset) {
            ThrowRuntime(ctx, std::string(who) + ": data segment is out of bounds");
            return nullptr;
        }
        if (d.bytes.size) std::memcpy(mem->data + offset, m.bytes.data() + d.bytes.offset, d.bytes.size);
        inst->dataDropped[i] = true;
    }
    if (m.hasStart) {
        uint64_t n[1] = {0};
        Value r[1] = {Value::null()};
        Machine& machine = Machine::Current();
        const Machine::Status status = machine.Invoke(ctx, inst->funcs[m.start], n, r);
        if (status == Machine::Status::Trap) { ThrowTrap(ctx, machine.lastTrap); return nullptr; }
        if (status == Machine::Status::Exception) return nullptr;
    }
    return inst;
}

// ---- The classes ----

Value ModuleConstructor(Context& ctx, Value, Args args, Value newTarget) {
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Module must be invoked with 'new'"); return Value(); }
    std::shared_ptr<Module> module;
    if (!Compile(ctx, args.empty() ? Value() : args[0], "WebAssembly.Module()", module)) return Value();
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kModuleKey);
    if (E::HasException(ctx)) return Value();
    ModuleObject* o = NewObjectOf<ModuleObject>(proto ? proto : R(ctx).moduleProto);
    o->module = std::move(module);
    return Value(static_cast<Object*>(o));
}

Value ModuleExports(Context& ctx, Value, Args args, Value) {
    ModuleObject* m = ModuleOf(ctx, args.empty() ? Value() : args[0], "WebAssembly.Module.exports()");
    if (!m) return Value();
    std::vector<Value> out;
    for (const Export& e : m->module->exports) {
        Value o = E::NewObject(ctx);
        E::Set(ctx, o, "name", E::NewString(ctx, e.name));
        E::Set(ctx, o, "kind", E::NewString(ctx, KindName(e.kind)));
        out.push_back(o);
    }
    return E::NewArray(ctx, Args(out.data(), out.size()));
}

Value ModuleImports(Context& ctx, Value, Args args, Value) {
    ModuleObject* m = ModuleOf(ctx, args.empty() ? Value() : args[0], "WebAssembly.Module.imports()");
    if (!m) return Value();
    std::vector<Value> out;
    for (const Import& im : m->module->imports) {
        Value o = E::NewObject(ctx);
        E::Set(ctx, o, "module", E::NewString(ctx, im.module));
        E::Set(ctx, o, "name", E::NewString(ctx, im.name));
        E::Set(ctx, o, "kind", E::NewString(ctx, KindName(im.kind)));
        out.push_back(o);
    }
    return E::NewArray(ctx, Args(out.data(), out.size()));
}

Value ModuleCustomSections(Context& ctx, Value, Args args, Value) {
    ModuleObject* m = ModuleOf(ctx, args.empty() ? Value() : args[0], "WebAssembly.Module.customSections()");
    if (!m) return Value();
    if (args.size() < 2 || args[1].is_undefined()) { ctx.throw_type_error("WebAssembly.Module.customSections(): Argument 1 is required"); return Value(); }
    const std::string name = E::ToUsvUtf8(ctx, args[1]);
    if (E::HasException(ctx)) return Value();
    std::vector<Value> out;
    for (const CustomSection& c : m->module->customs) {
        if (c.name != name) continue;
        Value buffer = E::NewArrayBuffer(ctx, c.bytes.size);
        if (c.bytes.size) {
            auto bytes = E::MutableBytesOf(buffer);
            if (bytes) std::memcpy(bytes->data(), m->module->bytes.data() + c.bytes.offset, c.bytes.size);
        }
        out.push_back(buffer);
    }
    return E::NewArray(ctx, Args(out.data(), out.size()));
}

Value InstanceConstructor(Context& ctx, Value, Args args, Value newTarget) {
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Instance must be invoked with 'new'"); return Value(); }
    ModuleObject* m = ModuleOf(ctx, args.empty() ? Value() : args[0], "WebAssembly.Instance()");
    if (!m) return Value();
    const Value imports = args.size() > 1 ? args[1] : Value();
    if (!imports.is_undefined() && !IsObjectLike(imports)) { ctx.throw_type_error("WebAssembly.Instance(): Argument 1 must be an object"); return Value(); }
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kInstanceKey);
    if (E::HasException(ctx)) return Value();
    InstanceObject* inst = Instantiate(ctx, m->module, imports, "WebAssembly.Instance()");
    if (!inst) return Value();
    if (proto) inst->set_prototype(proto);
    return Value(static_cast<Object*>(inst));
}

Value InstanceExports(Context& ctx, Value self, Args, Value) {
    InstanceObject* inst = DOMObject::Cast<InstanceObject>(self);
    if (!inst) { ctx.throw_type_error("WebAssembly.Instance.exports: Receiver is not a WebAssembly.Instance"); return Value(); }
    return inst->exports.Get();
}

Value MakeModuleObject(Context& ctx, std::shared_ptr<Module> module) {
    ModuleObject* o = NewObjectOf<ModuleObject>(R(ctx).moduleProto);
    o->module = std::move(module);
    return Value(static_cast<Object*>(o));
}

// A promise settled at once with what a call came to.
Value Settled(Context& ctx, bool ok, const Value& value) {
    E::PromiseCapability cap = E::NewPromiseCapability(ctx);
    E::Call(ctx, ok ? cap.resolve : cap.reject, Value(), Args(&value, 1));
    return cap.promise;
}

// Whatever was thrown, as the reason of a rejected promise.
Value Rejected(Context& ctx) {
    const Value reason = ctx.get_exception();
    ctx.clear_exception();
    return Settled(ctx, false, reason);
}

Value WasmValidate(Context& ctx, Value, Args args, Value) {
    const std::optional<std::span<const uint8_t>> bytes = E::BytesOf(args.empty() ? Value() : args[0]);
    if (!bytes) { ctx.throw_type_error("WebAssembly.validate(): Argument 0 must be a buffer source"); return Value(); }
    Module module;
    std::string error;
    return Value(Decode(bytes->data(), bytes->size(), module, error) && Validate(module, error));
}

Value WasmCompile(Context& ctx, Value, Args args, Value) {
    std::shared_ptr<Module> module;
    if (!Compile(ctx, args.empty() ? Value() : args[0], "WebAssembly.compile()", module)) return Rejected(ctx);
    return Settled(ctx, true, MakeModuleObject(ctx, std::move(module)));
}

Value WasmInstantiate(Context& ctx, Value, Args args, Value) {
    const Value first = args.empty() ? Value() : args[0];
    const Value imports = args.size() > 1 ? args[1] : Value();
    if (!imports.is_undefined() && !IsObjectLike(imports)) {
        ctx.throw_type_error("WebAssembly.instantiate(): Argument 1 must be an object");
        return Rejected(ctx);
    }
    if (ModuleObject* m = DOMObject::Cast<ModuleObject>(first)) {
        InstanceObject* inst = Instantiate(ctx, m->module, imports, "WebAssembly.instantiate()");
        if (!inst) return Rejected(ctx);
        return Settled(ctx, true, Value(static_cast<Object*>(inst)));
    }
    std::shared_ptr<Module> module;
    if (!Compile(ctx, first, "WebAssembly.instantiate()", module)) return Rejected(ctx);
    // The module is instantiated later, as it is when it has to be compiled first: the imports are not read before then.
    auto heldImports = std::make_shared<E::Persistent>(ctx, imports);
    Value step = E::NewFunction(ctx, "", 1, [module, heldImports](Context& c, Value, Args, Value) -> Value {
        InstanceObject* inst = Instantiate(c, module, heldImports->Get(), "WebAssembly.instantiate()");
        if (!inst) return Value();
        Value result = E::NewObject(c);
        E::Set(c, result, "module", MakeModuleObject(c, module));
        E::Set(c, result, "instance", Value(static_cast<Object*>(inst)));
        return result;
    });
    const Value start = Settled(ctx, true, Value());
    const Value then = E::Get(ctx, start, "then");
    return E::Call(ctx, then, start, Args(&step, 1));
}

// ---- Descriptors ----

bool ParseValType(Context& ctx, const std::string& s, ValType& out) {
    if (s == "i32") out = ValType::I32();
    else if (s == "i64") out = ValType::I64();
    else if (s == "f32") out = ValType::F32();
    else if (s == "f64") out = ValType::F64();
    else if (s == "v128") out = ValType::V128();
    else if (s == "externref") out = ValType::Ref(true, Heap::Extern);
    else if (s == "anyfunc" || s == "funcref") out = ValType::Ref(true, Heap::Func);
    else if (s == "anyref") out = ValType::Ref(true, Heap::Any);
    else if (s == "eqref") out = ValType::Ref(true, Heap::Eq);
    else if (s == "i31ref") out = ValType::Ref(true, Heap::I31);
    else if (s == "structref") out = ValType::Ref(true, Heap::Struct);
    else if (s == "arrayref") out = ValType::Ref(true, Heap::Array);
    else if (s == "exnref") out = ValType::Ref(true, Heap::Exn);
    else {
        (void)ctx;
        return false;
    }
    return true;
}

// An integer of a descriptor: a number or (for a 64-bit address) a BigInt. `present` says whether the member is there at all.
bool ReadSize(Context& ctx, const Value& descriptor, const char* name, bool is64, uint64_t& out, bool& present, const char* who) {
    const Value v = E::Get(ctx, descriptor, name);
    if (E::HasException(ctx)) return false;
    present = !v.is_undefined();
    if (!present) return true;
    if (v.is_bigint() && is64) {
        const BigInt& b = *v.as_bigint();
        if (b.is_negative()) { ctx.throw_type_error(std::string(who) + ": Property '" + name + "': value is below the minimum allowed value"); return false; }
        uint64_t bits;
        if (!ToInt64Bits(ctx, v, bits)) return false;
        out = bits;
        return true;
    }
    double d;
    if (!ToNumberChecked(ctx, v, d)) return false;
    if (std::isnan(d) || std::isinf(d)) { ctx.throw_type_error(std::string(who) + ": Property '" + name + "': value must be convertible to a valid number"); return false; }
    d = std::trunc(d);
    const double limit = is64 ? 18446744073709551615.0 : 4294967295.0;
    if (d < 0) { ctx.throw_type_error(std::string(who) + ": Property '" + name + "': value is below the minimum allowed value"); return false; }
    if (d > limit) { ctx.throw_type_error(std::string(who) + ": Property '" + name + "': value is above the maximum allowed value"); return false; }
    out = static_cast<uint64_t>(d);
    return true;
}

bool ReadAddressType(Context& ctx, const Value& descriptor, bool& is64) {
    const Value v = E::Get(ctx, descriptor, "address");
    if (E::HasException(ctx)) return false;
    is64 = false;
    if (v.is_undefined()) return true;
    const std::string s = E::ToUsvUtf8(ctx, v);
    if (E::HasException(ctx)) return false;
    if (s == "i32") return true;
    if (s == "i64") { is64 = true; return true; }
    ctx.throw_type_error("WebAssembly: invalid address type");
    return false;
}

// ---- Memory ----

Value MemoryConstructor(Context& ctx, Value, Args args, Value newTarget) {
    const char* who = "WebAssembly.Memory()";
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Memory must be invoked with 'new'"); return Value(); }
    const Value descriptor = args.empty() ? Value() : args[0];
    if (!IsObjectLike(descriptor)) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a memory descriptor"); return Value(); }
    bool is64;
    if (!ReadAddressType(ctx, descriptor, is64)) return Value();
    uint64_t initial = 0, maximum = 0;
    bool hasInitial, hasMax;
    if (!ReadSize(ctx, descriptor, "initial", is64, initial, hasInitial, who)) return Value();
    if (!ReadSize(ctx, descriptor, "maximum", is64, maximum, hasMax, who)) return Value();
    if (!hasInitial) { ctx.throw_type_error(std::string(who) + ": Property 'initial' is required"); return Value(); }
    const Value shared = E::Get(ctx, descriptor, "shared");
    if (E::HasException(ctx)) return Value();
    if (E::ToBoolean(shared)) { ctx.throw_type_error(std::string(who) + ": shared memory is not supported"); return Value(); }
    const uint64_t pageLimit = is64 ? (uint64_t(1) << 48) : 65536;
    if (initial > pageLimit) { ctx.throw_range_error(std::string(who) + ": Property 'initial': value " + std::to_string(initial) + " is above the upper bound " + std::to_string(pageLimit)); return Value(); }
    if (hasMax && maximum < initial) { ctx.throw_range_error(std::string(who) + ": Property 'maximum': value " + std::to_string(maximum) + " is below the lower bound " + std::to_string(initial)); return Value(); }
    if (hasMax && maximum > pageLimit) { ctx.throw_range_error(std::string(who) + ": Property 'maximum': value " + std::to_string(maximum) + " is above the upper bound " + std::to_string(pageLimit)); return Value(); }
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kMemoryKey);
    if (E::HasException(ctx)) return Value();
    MemType type;
    type.limits.min = initial;
    type.limits.max = maximum;
    type.limits.hasMax = hasMax;
    type.limits.is64 = is64;
    MemoryObject* m = NewObjectOf<MemoryObject>(proto ? proto : R(ctx).memoryProto);
    if (!m->Init(type)) { ctx.throw_range_error(std::string(who) + ": could not allocate memory"); return Value(); }
    return Value(static_cast<Object*>(m));
}

MemoryObject* ThisMemory(Context& ctx, const Value& self, const char* who) {
    MemoryObject* m = DOMObject::Cast<MemoryObject>(self);
    if (!m) ctx.throw_type_error(std::string(who) + ": Receiver is not a WebAssembly.Memory");
    return m;
}

Value MemoryBuffer(Context& ctx, Value self, Args, Value) {
    MemoryObject* m = ThisMemory(ctx, self, "WebAssembly.Memory.buffer");
    if (!m) return Value();
    const Value existing = m->buffer.Get();
    if (existing.is_object() && !E::IsDetached(existing)) return existing;
    Value buffer = E::NewArrayBuffer(ctx, m->data, static_cast<size_t>(m->Bytes()), [](void*, void*) {}, nullptr);
    m->buffer.Set(buffer);
    return buffer;
}

Value MemoryGrow(Context& ctx, Value self, Args args, Value) {
    MemoryObject* m = ThisMemory(ctx, self, "WebAssembly.Memory.grow()");
    if (!m) return Value();
    uint64_t delta = 0;
    bool present = false;
    if (args.empty()) { ctx.throw_type_error("WebAssembly.Memory.grow(): Argument 0 must be convertible to a valid number"); return Value(); }
    // (A descriptor-like read of the argument: the same conversion as the descriptors' members.)
    Value holder = E::NewObject(ctx);
    E::Set(ctx, holder, "delta", args[0]);
    if (!ReadSize(ctx, holder, "delta", m->type.limits.is64, delta, present, "WebAssembly.Memory.grow()")) return Value();
    if (!present) { ctx.throw_type_error("WebAssembly.Memory.grow(): Argument 0 must be convertible to a valid number"); return Value(); }
    const int64_t before = m->Grow(delta);
    if (before < 0) { ctx.throw_range_error("WebAssembly.Memory.grow(): failed to grow memory"); return Value(); }
    NotifyMemoryGrown(ctx, m);
    if (m->type.limits.is64) return FromInt64Bits(static_cast<uint64_t>(before));
    return Value(static_cast<double>(before));
}

// ---- Table ----

Value DefaultRef(const ValType& t) { return t.IsRef() && t.heap == Heap::Extern && false ? Value() : Value::null(); }

Value TableConstructor(Context& ctx, Value, Args args, Value newTarget) {
    const char* who = "WebAssembly.Table()";
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Table must be invoked with 'new'"); return Value(); }
    const Value descriptor = args.empty() ? Value() : args[0];
    if (!IsObjectLike(descriptor)) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a table descriptor"); return Value(); }
    const Value elementValue = E::Get(ctx, descriptor, "element");
    if (E::HasException(ctx)) return Value();
    const std::string element = E::ToUsvUtf8(ctx, elementValue);
    if (E::HasException(ctx)) return Value();
    ValType type;
    if (!ParseValType(ctx, element, type) || !type.IsRef()) { ctx.throw_type_error(std::string(who) + ": Descriptor property 'element' must be a WebAssembly reference type"); return Value(); }
    bool is64;
    if (!ReadAddressType(ctx, descriptor, is64)) return Value();
    uint64_t initial = 0, maximum = 0;
    bool hasInitial, hasMax;
    if (!ReadSize(ctx, descriptor, "initial", is64, initial, hasInitial, who)) return Value();
    if (!ReadSize(ctx, descriptor, "maximum", is64, maximum, hasMax, who)) return Value();
    if (!hasInitial) { ctx.throw_type_error(std::string(who) + ": Property 'initial' is required"); return Value(); }
    if (hasMax && maximum < initial) { ctx.throw_range_error(std::string(who) + ": Property 'maximum': value " + std::to_string(maximum) + " is below the lower bound " + std::to_string(initial)); return Value(); }
    if (initial > 10000000) { ctx.throw_range_error(std::string(who) + ": Property 'initial': value " + std::to_string(initial) + " is above the upper bound 10000000"); return Value(); }
    Value init = type.nullable && args.size() < 2 ? DefaultRef(type) : Value::null();
    if (args.size() >= 2 && !args[1].is_undefined()) {
        uint64_t n = 0;
        if (!ToWasm(ctx, args[1], type, &n, &init)) return Value();
    } else if (args.size() >= 2 && type.heap == Heap::Extern) {
        init = Value();
    }
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kTableKey);
    if (E::HasException(ctx)) return Value();
    TableType tt;
    tt.elem = type;
    tt.limits.min = initial;
    tt.limits.max = maximum;
    tt.limits.hasMax = hasMax;
    tt.limits.is64 = is64;
    TableObject* t = NewObjectOf<TableObject>(proto ? proto : R(ctx).tableProto);
    if (!t->Init(tt, init)) { ctx.throw_range_error(std::string(who) + ": could not allocate a table"); return Value(); }
    return Value(static_cast<Object*>(t));
}

TableObject* ThisTable(Context& ctx, const Value& self, const char* who) {
    TableObject* t = DOMObject::Cast<TableObject>(self);
    if (!t) ctx.throw_type_error(std::string(who) + ": Receiver is not a WebAssembly.Table");
    return t;
}

Value TableLength(Context& ctx, Value self, Args, Value) {
    TableObject* t = ThisTable(ctx, self, "WebAssembly.Table.length");
    if (!t) return Value();
    if (t->type.limits.is64) return FromInt64Bits(t->Size());
    return Value(static_cast<double>(t->Size()));
}

bool ReadIndex(Context& ctx, TableObject* t, const Value& v, uint64_t& out, const char* who) {
    Value holder = E::NewObject(ctx);
    E::Set(ctx, holder, "index", v);
    bool present = false;
    if (!ReadSize(ctx, holder, "index", t->type.limits.is64, out, present, who)) return false;
    if (!present) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be convertible to a valid number"); return false; }
    return true;
}

Value TableGet(Context& ctx, Value self, Args args, Value) {
    TableObject* t = ThisTable(ctx, self, "WebAssembly.Table.get()");
    if (!t) return Value();
    if (args.empty()) { ctx.throw_type_error("WebAssembly.Table.get(): Argument 0 must be convertible to a valid number"); return Value(); }
    uint64_t index;
    if (!ReadIndex(ctx, t, args[0], index, "WebAssembly.Table.get()")) return Value();
    if (index >= t->Size()) { ctx.throw_range_error("WebAssembly.Table.get(): invalid address " + std::to_string(index) + " in " + "table"); return Value(); }
    const Value ref = t->elems[static_cast<size_t>(index)];
    uint64_t n = 0;
    return ToJS(ctx, t->type.elem, &n, &ref);
}

Value TableSet(Context& ctx, Value self, Args args, Value) {
    TableObject* t = ThisTable(ctx, self, "WebAssembly.Table.set()");
    if (!t) return Value();
    if (args.empty()) { ctx.throw_type_error("WebAssembly.Table.set(): Argument 0 must be convertible to a valid number"); return Value(); }
    uint64_t index;
    if (!ReadIndex(ctx, t, args[0], index, "WebAssembly.Table.set()")) return Value();
    Value v = Value::null();
    if (args.size() >= 2) {
        uint64_t n = 0;
        if (!ToWasm(ctx, args[1], t->type.elem, &n, &v)) return Value();
    } else if (t->type.elem.heap == Heap::Extern) {
        v = Value();
    }
    if (index >= t->Size()) { ctx.throw_range_error("WebAssembly.Table.set(): invalid address " + std::to_string(index) + " in table"); return Value(); }
    t->elems.set(static_cast<size_t>(index), v);
    return Value();
}

Value TableGrow(Context& ctx, Value self, Args args, Value) {
    TableObject* t = ThisTable(ctx, self, "WebAssembly.Table.grow()");
    if (!t) return Value();
    if (args.empty()) { ctx.throw_type_error("WebAssembly.Table.grow(): Argument 0 must be convertible to a valid number"); return Value(); }
    uint64_t delta;
    if (!ReadIndex(ctx, t, args[0], delta, "WebAssembly.Table.grow()")) return Value();
    Value init = Value::null();
    if (args.size() >= 2 && !args[1].is_undefined()) {
        uint64_t n = 0;
        if (!ToWasm(ctx, args[1], t->type.elem, &n, &init)) return Value();
    } else if (args.size() >= 2 && t->type.elem.heap == Heap::Extern) {
        init = Value();
    }
    const int64_t before = t->Grow(delta, init);
    if (before < 0) { ctx.throw_range_error("WebAssembly.Table.grow(): failed to grow table by " + std::to_string(delta)); return Value(); }
    if (t->type.limits.is64) return FromInt64Bits(static_cast<uint64_t>(before));
    return Value(static_cast<double>(before));
}

// ---- Global ----

Value GlobalConstructor(Context& ctx, Value, Args args, Value newTarget) {
    const char* who = "WebAssembly.Global()";
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Global must be invoked with 'new'"); return Value(); }
    const Value descriptor = args.empty() ? Value() : args[0];
    if (!IsObjectLike(descriptor)) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a global descriptor"); return Value(); }
    const Value mutableValue = E::Get(ctx, descriptor, "mutable");
    if (E::HasException(ctx)) return Value();
    const Value typeValue = E::Get(ctx, descriptor, "value");
    if (E::HasException(ctx)) return Value();
    const std::string typeName = E::ToUsvUtf8(ctx, typeValue);
    if (E::HasException(ctx)) return Value();
    ValType type;
    if (!ParseValType(ctx, typeName, type)) { ctx.throw_type_error(std::string(who) + ": Descriptor property 'value' must be a WebAssembly type"); return Value(); }
    if (type.kind == ValType::Kind::V128) { ctx.throw_type_error(std::string(who) + ": type v128 is not allowed in the JavaScript API"); return Value(); }
    const Value v = args.size() > 1 ? args[1] : Value();
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kGlobalKey);
    if (E::HasException(ctx)) return Value();
    GlobalObject* g = NewObjectOf<GlobalObject>(proto ? proto : R(ctx).globalProto);
    g->type.type = type;
    g->type.mut = E::ToBoolean(mutableValue);
    uint64_t lo = 0;
    Value ref = Value::null();
    if (v.is_undefined()) {
        // (The default of the type; an externref that is not given is undefined.)
        if (type.IsRef() && type.heap == Heap::Extern) ref = args.size() > 1 ? Value() : Value();
        else if (type.IsRef() && !type.nullable) { ctx.throw_type_error(std::string(who) + ": Argument 1 is required for this type"); return Value(); }
    } else if (!ToWasm(ctx, v, type, &lo, &ref)) {
        return Value();
    }
    g->lo = lo;
    if (type.IsRef()) g->ref.Set(ref);
    return Value(static_cast<Object*>(g));
}

GlobalObject* ThisGlobal(Context& ctx, const Value& self, const char* who) {
    GlobalObject* g = DOMObject::Cast<GlobalObject>(self);
    if (!g) ctx.throw_type_error(std::string(who) + ": Receiver is not a WebAssembly.Global");
    return g;
}

Value GlobalValue(Context& ctx, Value self, Args, Value) {
    GlobalObject* g = ThisGlobal(ctx, self, "get WebAssembly.Global.value");
    if (!g) return Value();
    const Value ref = g->ref.Get();
    return ToJS(ctx, g->type.type, &g->lo, &ref);
}

Value GlobalSetValue(Context& ctx, Value self, Args args, Value) {
    GlobalObject* g = ThisGlobal(ctx, self, "set WebAssembly.Global.value");
    if (!g) return Value();
    if (!g->type.mut) { ctx.throw_type_error("set WebAssembly.Global.value: Can't set the value of an immutable global."); return Value(); }
    uint64_t lo = 0;
    Value ref = Value::null();
    if (!ToWasm(ctx, args.empty() ? Value() : args[0], g->type.type, &lo, &ref)) return Value();
    g->lo = lo;
    if (g->type.type.IsRef()) g->ref.Set(ref);
    return Value();
}

Value GlobalValueOf(Context& ctx, Value self, Args, Value) { return GlobalValue(ctx, self, {}, Value()); }

// ---- Tag ----

Value TagConstructor(Context& ctx, Value, Args args, Value newTarget) {
    const char* who = "WebAssembly.Tag()";
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Tag must be invoked with 'new'"); return Value(); }
    const Value descriptor = args.empty() ? Value() : args[0];
    if (!IsObjectLike(descriptor)) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a tag type"); return Value(); }
    const Value params = E::Get(ctx, descriptor, "parameters");
    if (E::HasException(ctx)) return Value();
    if (!IsObjectLike(params)) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a tag type with 'parameters'"); return Value(); }
    const Value lengthValue = E::Get(ctx, params, "length");
    if (E::HasException(ctx)) return Value();
    const double length = E::ToNumber(ctx, lengthValue);
    if (E::HasException(ctx)) return Value();
    Module module;
    TypeDef def;
    def.kind = TypeDef::Kind::Func;
    for (uint32_t i = 0; i < static_cast<uint32_t>(length); ++i) {
        const Value item = E::GetIndex(ctx, params, i);
        if (E::HasException(ctx)) return Value();
        const std::string name = E::ToUsvUtf8(ctx, item);
        if (E::HasException(ctx)) return Value();
        ValType t;
        if (!ParseValType(ctx, name, t)) { ctx.throw_type_error(std::string(who) + ": Argument 0 parameter type at index #" + std::to_string(i) + " must be a value type"); return Value(); }
        if (t.kind == ValType::Kind::V128) { ctx.throw_type_error(std::string(who) + ": type v128 is not allowed in the JavaScript API"); return Value(); }
        def.func.params.push_back(t);
    }
    module.types.push_back(def);
    std::vector<uint32_t> ids;
    std::string error;
    if (!TypeRegistry::Canonicalize(module, ids, error)) { ctx.throw_type_error(std::string(who) + ": " + error); return Value(); }
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kTagKey);
    if (E::HasException(ctx)) return Value();
    TagObject* t = NewObjectOf<TagObject>(proto ? proto : R(ctx).tagProto);
    t->sig = ids[0];
    return Value(static_cast<Object*>(t));
}

// ---- Exception ----

uint32_t SignatureOf(const std::vector<ValType>& params, std::string& error) {
    Module module;
    TypeDef def;
    def.kind = TypeDef::Kind::Func;
    def.func.params = params;
    module.types.push_back(def);
    std::vector<uint32_t> ids;
    if (!TypeRegistry::Canonicalize(module, ids, error)) return 0;
    return ids[0];
}

Value ExceptionConstructor(Context& ctx, Value, Args args, Value newTarget) {
    const char* who = "WebAssembly.Exception()";
    if (newTarget.is_undefined()) { ctx.throw_type_error("WebAssembly.Exception must be invoked with 'new'"); return Value(); }
    TagObject* tag = DOMObject::Cast<TagObject>(args.empty() ? Value() : args[0]);
    if (!tag) { ctx.throw_type_error(std::string(who) + ": Argument 0 must be a WebAssembly tag"); return Value(); }
    const Value payload = args.size() > 1 ? args[1] : Value();
    if (!IsObjectLike(payload)) { ctx.throw_type_error(std::string(who) + ": Argument 1 must be an iterable"); return Value(); }
    const std::vector<ValType>& params = TypeRegistry::Get(tag->sig).func.params;
    std::vector<Value> items;
    if (!IterateToVector(ctx, payload, items)) return Value();
    if (items.size() != params.size()) { ctx.throw_type_error(std::string(who) + ": Number of exception values does not match signature length"); return Value(); }
    Object* proto = E::PrototypeFromNewTarget(ctx, newTarget, &kExceptionKey);
    if (E::HasException(ctx)) return Value();
    std::vector<uint64_t> numbers;
    std::vector<Value> refs;
    for (size_t i = 0; i < params.size(); ++i) {
        const Value item = items[i];
        uint64_t n[2] = {0, 0};
        Value r = Value::null();
        if (!ToWasm(ctx, item, params[i], n, &r)) return Value();
        if (params[i].IsRef()) refs.push_back(r);
        else numbers.push_back(n[0]);
    }
    Value traceOptions = args.size() > 2 ? args[2] : Value();
    bool traceStack = false;
    if (IsObjectLike(traceOptions)) {
        const Value t = E::Get(ctx, traceOptions, "traceStack");
        if (E::HasException(ctx)) return Value();
        traceStack = E::ToBoolean(t);
    }
    ExceptionObject* e = NewObjectOf<ExceptionObject>(proto ? proto : R(ctx).exceptionProto);
    e->tag.Set(Value(static_cast<Object*>(tag)));
    e->numbers = std::move(numbers);
    for (const Value& r : refs) e->refs.push_back(r);
    if (traceStack) E::Set(ctx, Value(static_cast<Object*>(e)), "stack", E::NewString(ctx, "WebAssembly.Exception"));
    return Value(static_cast<Object*>(e));
}

Value ExceptionIs(Context& ctx, Value self, Args args, Value) {
    ExceptionObject* e = DOMObject::Cast<ExceptionObject>(self);
    if (!e) { ctx.throw_type_error("WebAssembly.Exception.is(): Receiver is not a WebAssembly.Exception"); return Value(); }
    TagObject* tag = DOMObject::Cast<TagObject>(args.empty() ? Value() : args[0]);
    if (!tag) { ctx.throw_type_error("WebAssembly.Exception.is(): Argument 0 must be a WebAssembly tag"); return Value(); }
    return Value(DOMObject::Cast<TagObject>(e->tag.Get()) == tag);
}

Value ExceptionGetArg(Context& ctx, Value self, Args args, Value) {
    ExceptionObject* e = DOMObject::Cast<ExceptionObject>(self);
    if (!e) { ctx.throw_type_error("WebAssembly.Exception.getArg(): Receiver is not a WebAssembly.Exception"); return Value(); }
    TagObject* tag = DOMObject::Cast<TagObject>(args.empty() ? Value() : args[0]);
    if (!tag) { ctx.throw_type_error("WebAssembly.Exception.getArg(): Argument 0 must be a WebAssembly tag"); return Value(); }
    if (DOMObject::Cast<TagObject>(e->tag.Get()) != tag) { ctx.throw_type_error("WebAssembly.Exception.getArg(): First argument does not match the exception tag"); return Value(); }
    if (args.size() < 2) { ctx.throw_type_error("WebAssembly.Exception.getArg(): Argument 1 must be convertible to a valid number"); return Value(); }
    double index;
    if (!ToNumberChecked(ctx, args[1], index)) return Value();
    const std::vector<ValType>& params = TypeRegistry::Get(tag->sig).func.params;
    if (!(index >= 0) || index >= static_cast<double>(params.size()) || index != std::trunc(index)) { ctx.throw_range_error("WebAssembly.Exception.getArg(): Index out of range"); return Value(); }
    size_t ni = 0, ri = 0;
    for (size_t i = 0; i < static_cast<size_t>(index); ++i) { if (params[i].IsRef()) ++ri; else ++ni; }
    const ValType& t = params[static_cast<size_t>(index)];
    uint64_t zero = 0;
    Value nothing = Value::null();
    if (t.IsRef()) return ToJS(ctx, t, &zero, &e->refs[ri]);
    return ToJS(ctx, t, &e->numbers[ni], &nothing);
}

// ---- The errors ----

Value MakeErrorClass(Context& ctx, const char* name, Value (*constructor)(Context&, Value, Args, Value), Value& out) {
    const E::ClassRef ref = E::DefineClass(ctx, name, constructor, 1, nullptr);
    out = Value(ref.constructor);
    return Value(ref.constructor);
}

Value ErrorConstructorFor(Context& ctx, Value ctor, Args args, Value newTarget, const char* name) {
    // (A call without new makes one just the same.)
    Value target = newTarget.is_undefined() ? ctor : newTarget;
    const Value errorCtor = E::Get(ctx, Value(static_cast<Object*>(ctx.get_global_object())), "Error");
    if (E::HasException(ctx)) return Value();
    (void)name;
    return E::Construct(ctx, errorCtor, args, target);
}

}  // namespace

void register_webassembly_builtins(Context& ctx) {
    RealmData* data = new RealmData();
    E::SetRealmData(ctx, &kRealmKey, data);

    Value ns = E::NewObject(ctx);
    E::DefineToStringTag(ns.as_object(), "WebAssembly");

    const auto attach = [&](const char* name, Object* ctor) {
        E::Descriptor d;
        d.value = Value(ctor); d.has_value = true;
        d.has_writable = true; d.writable = true;
        d.has_enumerable = true; d.enumerable = false;
        d.has_configurable = true; d.configurable = true;
        E::DefineProperty(ctx, ns, name, d);
    };

    {
        E::ClassRef c = E::DefineClass(ctx, "Module", ModuleConstructor, 1);
        data->moduleProto = c.prototype;
        E::SetRealmData(ctx, &kModuleKey, c.prototype);
        E::DefineStaticMethod(c.constructor, "exports", ModuleExports, 1);
        E::DefineStaticMethod(c.constructor, "imports", ModuleImports, 1);
        E::DefineStaticMethod(c.constructor, "customSections", ModuleCustomSections, 2);
        attach("Module", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Instance", InstanceConstructor, 1);
        data->instanceProto = c.prototype;
        E::SetRealmData(ctx, &kInstanceKey, c.prototype);
        E::DefineAccessor(c.prototype, "exports", InstanceExports, nullptr);
        attach("Instance", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Memory", MemoryConstructor, 1);
        data->memoryProto = c.prototype;
        E::SetRealmData(ctx, &kMemoryKey, c.prototype);
        E::DefineMethod(c.prototype, "grow", MemoryGrow, 1);
        E::DefineAccessor(c.prototype, "buffer", MemoryBuffer, nullptr);
        attach("Memory", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Table", TableConstructor, 1);
        data->tableProto = c.prototype;
        E::SetRealmData(ctx, &kTableKey, c.prototype);
        E::DefineAccessor(c.prototype, "length", TableLength, nullptr);
        E::DefineMethod(c.prototype, "grow", TableGrow, 1);
        E::DefineMethod(c.prototype, "get", TableGet, 1);
        E::DefineMethod(c.prototype, "set", TableSet, 1);
        attach("Table", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Global", GlobalConstructor, 1);
        data->globalProto = c.prototype;
        E::SetRealmData(ctx, &kGlobalKey, c.prototype);
        E::DefineMethod(c.prototype, "valueOf", GlobalValueOf, 0);
        E::DefineAccessor(c.prototype, "value", GlobalValue, GlobalSetValue);
        attach("Global", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Tag", TagConstructor, 1);
        data->tagProto = c.prototype;
        E::SetRealmData(ctx, &kTagKey, c.prototype);
        attach("Tag", c.constructor);
    }
    {
        E::ClassRef c = E::DefineClass(ctx, "Exception", ExceptionConstructor, 2);
        data->exceptionProto = c.prototype;
        E::SetRealmData(ctx, &kExceptionKey, c.prototype);
        E::DefineMethod(c.prototype, "is", ExceptionIs, 1);
        E::DefineMethod(c.prototype, "getArg", ExceptionGetArg, 2);
        attach("Exception", c.constructor);
    }
    for (const auto& [proto, tag] : {std::pair<Object*, const char*>{data->moduleProto, "WebAssembly.Module"}, {data->instanceProto, "WebAssembly.Instance"}, {data->memoryProto, "WebAssembly.Memory"},
                                     {data->tableProto, "WebAssembly.Table"}, {data->globalProto, "WebAssembly.Global"}, {data->tagProto, "WebAssembly.Tag"}, {data->exceptionProto, "WebAssembly.Exception"}}) {
        E::DefineToStringTag(proto, tag);
    }
    {
        // WebAssembly.JSTag: the tag of what JavaScript throws, as it is when it passes through a module.
        std::string error;
        TagObject* t = NewObjectOf<TagObject>(data->tagProto);
        t->sig = SignatureOf({ValType::Ref(true, Heap::Extern)}, error);
        data->jsTag = t;
        E::Descriptor d;
        d.value = Value(static_cast<Object*>(t)); d.has_value = true;
        d.has_writable = true; d.writable = false;
        d.has_enumerable = true; d.enumerable = false;
        d.has_configurable = true; d.configurable = false;
        E::DefineProperty(ctx, ns, "JSTag", d);
    }
    const Value errorProtoHolder = E::Get(ctx, Value(static_cast<Object*>(ctx.get_global_object())), "Error");
    Object* errorProto = nullptr;
    if (errorProtoHolder.is_function()) {
        const Value p = E::Get(ctx, errorProtoHolder, "prototype");
        if (p.is_object()) errorProto = p.as_object();
    }
    const auto errorClass = [&](const char* name, E::NativeFn fn, Value& slot) {
        E::ClassRef c = E::DefineClass(ctx, name, fn, 1, errorProto);
        slot = Value(c.constructor);
        // (An error class is a constructor of Error: its prototype chain goes through it.)
        E::SetPrototypeOf(ctx, slot, errorProtoHolder);
        E::DefineProperty(ctx, Value(c.prototype), "name", [&] { E::Descriptor d; d.value = E::NewString(ctx, name); d.has_value = true; d.has_writable = true; d.writable = true; d.has_enumerable = true; d.enumerable = false; d.has_configurable = true; d.configurable = true; return d; }());
        E::DefineProperty(ctx, Value(c.prototype), "message", [&] { E::Descriptor d; d.value = E::NewString(ctx, ""); d.has_value = true; d.has_writable = true; d.writable = true; d.has_enumerable = true; d.enumerable = false; d.has_configurable = true; d.configurable = true; return d; }());
        attach(name, c.constructor);
    };
    errorClass("CompileError", [](Context& c, Value, Args a, Value nt) { return ErrorConstructorFor(c, R(c).compileError, a, nt, "CompileError"); }, data->compileError);
    errorClass("LinkError", [](Context& c, Value, Args a, Value nt) { return ErrorConstructorFor(c, R(c).linkError, a, nt, "LinkError"); }, data->linkError);
    errorClass("RuntimeError", [](Context& c, Value, Args a, Value nt) { return ErrorConstructorFor(c, R(c).runtimeError, a, nt, "RuntimeError"); }, data->runtimeError);

    E::DefineStaticMethod(ns.as_object(), "validate", WasmValidate, 1);
    E::DefineStaticMethod(ns.as_object(), "compile", WasmCompile, 1);
    E::DefineStaticMethod(ns.as_object(), "instantiate", WasmInstantiate, 1);
    E::DefineGlobal(ctx, "WebAssembly", ns.as_object());
}

}  // namespace Quanta::Wasm
