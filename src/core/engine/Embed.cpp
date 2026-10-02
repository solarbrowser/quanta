/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/Embed.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Symbol.h"

#include <cmath>

namespace Quanta::Embed {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The WHATWG UTF-8 decoder, re-encoding as it goes: one U+FFFD per maximal
// invalid subpart, which is what a conforming decoder produces and so what
// script observes through TextDecoder. With `wtf8`, a surrogate encoded as a
// 3-byte sequence is accepted as the engine stores lone surrogates, a high
// surrogate directly followed by a low one is merged back into the pair it
// was, and any surrogate left unpaired becomes U+FFFD -- the USVString rule.
std::string to_scalar_values(std::string_view in, bool wtf8) {
    std::string out;
    out.reserve(in.size());

    uint32_t cp = 0;
    int needed = 0, seen = 0;
    uint8_t lower = 0x80, upper = 0xBF;
    uint32_t pending_high = 0;

    auto emit = [&](uint32_t c) {
        if (pending_high) {
            uint32_t high = pending_high;
            pending_high = 0;
            if (c >= 0xDC00 && c <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((high - 0xD800) << 10) + (c - 0xDC00));
                return;
            }
            append_utf8(out, kReplacement);
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            pending_high = c;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            append_utf8(out, kReplacement);
        } else {
            append_utf8(out, c);
        }
    };

    for (size_t i = 0; i < in.size();) {
        uint8_t b = static_cast<uint8_t>(in[i]);
        if (needed == 0) {
            i++;
            if (b <= 0x7F) {
                emit(b);
            } else if (b >= 0xC2 && b <= 0xDF) {
                needed = 1; cp = b & 0x1F;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0) lower = 0xA0;
                if (b == 0xED && !wtf8) upper = 0x9F;
                needed = 2; cp = b & 0x0F;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0) lower = 0x90;
                if (b == 0xF4) upper = 0x8F;
                needed = 3; cp = b & 0x07;
            } else {
                emit(kReplacement);
            }
            continue;
        }
        if (b < lower || b > upper) {
            cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF;
            emit(kReplacement);
            continue;
        }
        i++;
        lower = 0x80; upper = 0xBF;
        cp = (cp << 6) | (b & 0x3F);
        if (++seen == needed) {
            uint32_t done = cp;
            cp = 0; needed = 0; seen = 0;
            emit(done);
        }
    }
    if (needed != 0) emit(kReplacement);
    if (pending_high) append_utf8(out, kReplacement);
    return out;
}

std::unique_ptr<Function> make_native(const char* name, NativeFn fn, int length) {
    return ObjectFactory::create_native_function(name,
        [fn](Context& ctx, std::span<const Value> args, Value receiver) -> Value {
            return fn(ctx, receiver, args, Value());
        },
        static_cast<uint32_t>(length));
}

}

// ---- Lifecycle ------------------------------------------------------------

std::unique_ptr<Runtime> Runtime::Create() {
    std::unique_ptr<Runtime> rt(new Runtime());
    Engine::Config config;
    config.host_drives_event_loop = true;
    rt->engine_ = std::make_unique<Engine>(config);
    if (!rt->engine_->initialize()) return nullptr;
    return rt;
}

Runtime::~Runtime() {
    if (!engine_) return;
    Heap* heap = engine_->get_heap();
    // A major cycle is incremental and can be open right now, with this engine's
    // contexts and environments queued to be traced. Let it end while they are
    // still there: tracing them after they are freed would corrupt the marks.
    if (Collector::major_in_progress()) Collector::collect();
    // Contexts next (they name cells), then the cells: nothing the runtime built
    // is reachable once it is gone, and a collection that still traced one would
    // follow its pointers into freed contexts.
    engine_.reset();
    Collector::retire_heap(heap);
}

Context& Runtime::GetContext() {
    return *engine_->get_global_context();
}

Runtime::Result Runtime::Evaluate(std::string_view source, const std::string& filename) {
    Engine::Result r = engine_->execute(std::string(source), filename);
    Result out;
    out.ok = r.success;
    out.exception = r.exception_value;
    out.error = r.error_message;
    return out;
}

void Runtime::CollectGarbage() {
    engine_->force_gc();
}

void Runtime::PerformMicrotaskCheckpoint() {
    HeapScope heap_scope(engine_->get_heap());
    Context& ctx = GetContext();
    ctx.drain_microtasks();
    Promise::report_unhandled_rejections();
}

bool Runtime::RunDueTimers() {
    HeapScope heap_scope(engine_->get_heap());
    return EventLoop::instance().run_due_timers();
}

std::optional<int64_t> Runtime::NextTimerDelayMs() {
    auto delay = EventLoop::instance().next_timer_delay();
    if (!delay) return std::nullopt;
    return delay->count();
}

// ---- Exposing a class to script -------------------------------------------

ClassRef DefineClass(Context& ctx, const char* name, NativeFn constructor, int length, Object* parentProto) {
    auto proto = ObjectFactory::create_object(parentProto);
    Object* proto_ptr = proto.get();

    auto ctor = ObjectFactory::create_native_constructor_with_new_target(name,
        [constructor](Context& ctx, std::span<const Value> args, Value receiver,
                      bool is_construct, Value new_target) -> Value {
            return constructor(ctx, receiver, args, is_construct ? new_target : Value());
        },
        static_cast<uint32_t>(length));
    Function* ctor_ptr = ctor.get();

    proto->set_property("constructor", Value(ctor_ptr), PropertyAttributes::BuiltinFunction);
    DefineToStringTag(proto_ptr, name);
    ctor->set_property("prototype", Value(proto.release()), PropertyAttributes::None);
    ctor.release();
    // The host keeps ClassRef's pointers in C++ variables the collector cannot
    // see, and an interface script never names (an iterator class) would
    // otherwise be swept. The prototype is kept by the constructor's own
    // non-configurable "prototype" property.
    ctx.root_built_in_object(std::string("@@embed:") + name + "@" +
                             std::to_string(reinterpret_cast<uintptr_t>(ctor_ptr)), ctor_ptr);
    return {ctor_ptr, proto_ptr};
}

void DefineMethod(Object* proto, const char* name, NativeFn fn, int length) {
    proto->set_property(name, Value(make_native(name, fn, length).release()), PropertyAttributes::Default);
}

void DefineStaticMethod(Object* constructor, const char* name, NativeFn fn, int length) {
    constructor->set_property(name, Value(make_native(name, fn, length).release()), PropertyAttributes::Default);
}

void DefineAccessor(Object* proto, const char* name, NativeFn getter, NativeFn setter) {
    PropertyDescriptor desc;
    desc.set_getter(make_native((std::string("get ") + name).c_str(), getter, 0).release());
    if (setter) desc.set_setter(make_native((std::string("set ") + name).c_str(), setter, 1).release());
    desc.set_enumerable(true);
    desc.set_configurable(true);
    proto->set_property_descriptor(name, desc);
}

void DefineToStringTag(Object* proto, const char* tag) {
    Symbol* sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (!sym) return;
    PropertyDescriptor desc(Value(std::string(tag)), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    proto->set_property_descriptor(sym->to_property_key(), desc);
}

void DefineGlobal(Context& ctx, const char* name, Object* constructor) {
    ctx.register_built_in_object(name, constructor);
}

Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget) {
    Object* target = newTarget.as_object_or_null();
    if (!target) return nullptr;
    Value proto = target->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    return proto.as_object_or_null();
}

// ---- Values ---------------------------------------------------------------

uint32_t ToUint32(Context& ctx, const Value& v) {
    double d = 0;
    if (!v.to_number_checked(ctx, d)) return 0;
    if (!std::isfinite(d)) return 0;
    d = std::fmod(std::trunc(d), 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return static_cast<uint32_t>(d);
}

std::string ToUsvUtf8(Context& ctx, const Value& v) {
    std::string s;
    if (!v.to_string_checked(ctx, s)) return {};
    return to_scalar_values(s, /*wtf8=*/true);
}

Value FromUtf8(Context&, std::string_view utf8) {
    return Value(to_scalar_values(utf8, /*wtf8=*/false));
}

// ---- Arrays and properties ------------------------------------------------

Value NewArray(Context&) {
    return Value(ObjectFactory::create_array(0).release());
}

void ArrayPush(Context&, const Value& array, const Value& element) {
    if (Object* a = array.as_object_or_null()) a->push(element);
}

Value Get(Context& ctx, const Value& object, std::string_view name) {
    Object* o = object.as_object_or_null();
    if (!o) {
        ctx.throw_type_error("Cannot read properties of a non-object");
        return Value();
    }
    return o->get_property(std::string(name));
}

bool Set(Context& ctx, const Value& object, std::string_view name, const Value& value) {
    Object* o = object.as_object_or_null();
    if (!o) {
        ctx.throw_type_error("Cannot set properties of a non-object");
        return false;
    }
    return o->set_property(std::string(name), value);
}

std::vector<std::string> OwnKeys(Context& ctx, const Value& object) {
    Object* o = object.as_object_or_null();
    if (!o) {
        ctx.throw_type_error("Cannot enumerate a non-object");
        return {};
    }
    // Symbols are stored under string keys of this shape (Symbol::
    // to_property_key), and the engine's own key list does not separate them.
    std::vector<std::string> keys = o->get_enumerable_keys();
    std::erase_if(keys, [](const std::string& k) {
        return k.starts_with("@@sym:") || k.starts_with("Symbol.");
    });
    return keys;
}

// ---- Calling back into script ---------------------------------------------

Value Call(Context& ctx, const Value& callable, const Value& thisValue, Args args) {
    if (!callable.is_function()) {
        ctx.throw_type_error("Value is not callable");
        return Value();
    }
    return callable.as_function()->call(ctx, std::vector<Value>(args.begin(), args.end()), thisValue);
}

// ---- Iterators ------------------------------------------------------------

Object* GetIteratorPrototype(Context& ctx) {
    Object* iterator = ctx.get_built_in_object("Iterator");
    if (!iterator) return nullptr;
    return iterator->get_property("prototype").as_object_or_null();
}

Value GetIteratorMethod(Context& ctx, const Value& object) {
    Object* target = object.as_object_or_null();
    if (!target) {
        if (object.is_nullish()) {
            ctx.throw_type_error("Cannot read @@iterator of null or undefined");
            return Value();
        }
        // A primitive is looked up on its realm's intrinsic prototype, which is
        // where its wrapper would find it.
        Context::PrimitiveKind kind = object.is_string()    ? Context::PrimitiveKind::String
                                      : object.is_number()  ? Context::PrimitiveKind::Number
                                      : object.is_boolean() ? Context::PrimitiveKind::Boolean
                                      : object.is_bigint()  ? Context::PrimitiveKind::BigInt
                                                            : Context::PrimitiveKind::Symbol;
        target = Context::primitive_prototype(kind);
        if (!target) return Value();
    }
    Symbol* iterator = Symbol::get_well_known(Symbol::ITERATOR);
    if (!iterator) return Value();
    Value method = target->get_property(iterator->to_property_key());
    if (ctx.has_exception()) return Value();
    if (method.is_nullish()) return Value();
    if (!method.is_function()) {
        ctx.throw_type_error("@@iterator is not a function");
        return Value();
    }
    return method;
}

Value MakeIterResult(Context& ctx, const Value& value, bool done) {
    return Iterator::create_iterator_result(value, done, &ctx);
}

// ---- Promises -------------------------------------------------------------

PromiseCapability NewPromiseCapability(Context& ctx) {
    Object* promise_ctor = ctx.get_built_in_object("Promise");
    if (!promise_ctor) {
        ctx.throw_type_error("Promise is not available");
        return {};
    }
    Value pair = Promise::withResolvers(ctx, {}, Value(promise_ctor));
    if (ctx.has_exception()) return {};
    return {Get(ctx, pair, "promise"), Get(ctx, pair, "resolve"), Get(ctx, pair, "reject")};
}

// ---- Memory ---------------------------------------------------------------

void ReportExternalAllocation(size_t bytes) {
    Heap::note_offheap_bytes(bytes);
}

// ---- Errors ---------------------------------------------------------------

void ThrowTypeError(Context& ctx, const std::string& message) {
    ctx.throw_type_error(message);
}

}
