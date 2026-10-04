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
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/Symbol.h"

#include <cmath>
#include <cstring>

namespace Quanta::Embed {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

// Engine code that has no Context in hand (resolving a promise with a thenable, making a
// promise) reads the one running script's. A host entering from outside any script has none
// yet, so the context it is calling through stands in for the duration.
class RunningContext {
public:
    explicit RunningContext(Context& ctx) : previous_(Object::current_context_) { Object::current_context_ = &ctx; }
    ~RunningContext() { Object::current_context_ = previous_; }
    RunningContext(const RunningContext&) = delete;
    RunningContext& operator=(const RunningContext&) = delete;
private:
    Context* previous_;
};

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
enum class Surrogates { Replace, Keep };

// With Keep, a lone surrogate is stored as the engine stores one (a 3-byte sequence) instead of
// becoming U+FFFD, which is what a DOMString needs: it is a sequence of code units, not scalar
// values. A high surrogate directly followed by a low one is still one code point.
std::string to_scalar_values(std::string_view in, bool wtf8, Surrogates surrogates = Surrogates::Replace) {
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
            append_utf8(out, surrogates == Surrogates::Keep ? high : kReplacement);
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            pending_high = c;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            append_utf8(out, surrogates == Surrogates::Keep ? c : kReplacement);
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
    if (pending_high) append_utf8(out, surrogates == Surrogates::Keep ? pending_high : kReplacement);
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

namespace {
constinit thread_local bool g_isolate_live = false;
}

Realm::Realm(Isolate& isolate, std::unique_ptr<Engine> engine)
    : isolate_(&isolate), engine_(std::move(engine)) {}

Realm::~Realm() {
    if (!engine_) return;
    engine_->set_host_realm(nullptr);
    if (isolate_) {
        std::erase(isolate_->realms_, this);
    }
    // A major cycle is incremental and can be open right now, with this engine's
    // contexts and environments queued to be traced. Let it end while they are
    // still there: tracing them after they are freed would corrupt the marks.
    if (Collector::major_in_progress()) Collector::collect();
    engine_.reset();
}

Context& Realm::GetContext() {
    return *engine_->get_global_context();
}

void Realm::Run(const std::function<void()>& body) {
    HeapScope heap_scope(engine_->get_heap());
    RealmScope realm_scope(engine_->realm());
    RunningContext running(GetContext());
    body();
}

Realm* Realm::FromContext(Context& ctx) {
    Quanta::Realm* realm = ctx.realm();
    Engine* engine = realm ? realm->engine() : nullptr;
    return engine ? static_cast<Realm*>(engine->host_realm()) : nullptr;
}

EvaluateResult Realm::Evaluate(std::string_view source, const std::string& filename) {
    Engine::Result r = engine_->execute(std::string(source), filename);
    EvaluateResult out;
    out.ok = r.success;
    out.exception = r.exception_value;
    out.error = r.error_message;
    return out;
}

std::unique_ptr<Isolate> Isolate::Create() {
    if (g_isolate_live) return nullptr;
    std::unique_ptr<Isolate> isolate(new Isolate());
    isolate->isolate_ = std::make_unique<Quanta::Isolate>();
    g_isolate_live = true;
    return isolate;
}

Isolate::~Isolate() {
    if (!isolate_) return;
    // The realms the host still holds go with the Isolate; their owners find them
    // empty and have nothing to free.
    for (Realm* realm : std::vector<Realm*>(realms_)) {
        if (Collector::major_in_progress()) Collector::collect();
        realm->engine_->set_host_realm(nullptr);
        realm->engine_.reset();
        realm->isolate_ = nullptr;
    }
    realms_.clear();
    isolate_.reset();
    g_isolate_live = false;
}

std::unique_ptr<Realm> Isolate::CreateRealm() {
    Engine::Config config;
    config.host_drives_event_loop = true;
    auto engine = std::make_unique<Engine>(*isolate_, config);
    if (!engine->initialize()) return nullptr;
    std::unique_ptr<Realm> realm(new Realm(*this, std::move(engine)));
    realm->engine_->set_host_realm(realm.get());
    realms_.push_back(realm.get());
    return realm;
}

void Isolate::CollectGarbage() {
    HeapScope heap_scope(isolate_->heap());
    Collector::collect();
}

void Isolate::PerformMicrotaskCheckpoint() {
    HeapScope heap_scope(isolate_->heap());
    // Each job runs in the realm that queued it; this is for the ones that did not
    // say, and for what runs after the last.
    RealmScope realm_scope(realms_.empty() ? nullptr : realms_.front()->engine_->realm());
    std::optional<RunningContext> running;
    if (!realms_.empty()) running.emplace(realms_.front()->GetContext());
    EventLoop::instance().drain_microtasks();
    Promise::report_unhandled_rejections();
}

bool Isolate::RunDueTimers() {
    HeapScope heap_scope(isolate_->heap());
    RealmScope realm_scope(realms_.empty() ? nullptr : realms_.front()->engine_->realm());
    std::optional<RunningContext> running;
    if (!realms_.empty()) running.emplace(realms_.front()->GetContext());
    return EventLoop::instance().run_due_timers();
}

std::optional<int64_t> Isolate::NextTimerDelayMs() {
    auto delay = EventLoop::instance().next_timer_delay();
    if (!delay) return std::nullopt;
    return delay->count();
}

std::unique_ptr<Runtime> Runtime::Create() {
    std::unique_ptr<Runtime> rt(new Runtime());
    rt->isolate_ = Isolate::Create();
    if (!rt->isolate_) return nullptr;
    rt->realm_ = rt->isolate_->CreateRealm();
    if (!rt->realm_) return nullptr;
    return rt;
}

Runtime::~Runtime() {
    // The realm first, then the Isolate, whose heap is retired by one collection
    // that frees everything the runtime built.
    realm_.reset();
    isolate_.reset();
}

Context& Runtime::GetContext() { return realm_->GetContext(); }

Runtime::Result Runtime::Evaluate(std::string_view source, const std::string& filename) {
    return realm_->Evaluate(source, filename);
}

void Runtime::CollectGarbage() { isolate_->CollectGarbage(); }
void Runtime::PerformMicrotaskCheckpoint() { isolate_->PerformMicrotaskCheckpoint(); }
bool Runtime::RunDueTimers() { return isolate_->RunDueTimers(); }
std::optional<int64_t> Runtime::NextTimerDelayMs() { return isolate_->NextTimerDelayMs(); }

// ---- Per-realm data -------------------------------------------------------

void SetRealmData(Context& ctx, const void* key, void* value) {
    if (Quanta::Realm* realm = ctx.realm()) realm->embedder_data[key] = value;
}

void* GetRealmData(Context& ctx, const void* key) {
    Quanta::Realm* realm = g_current_realm ? g_current_realm : ctx.realm();
    if (!realm) return nullptr;
    auto it = realm->embedder_data.find(key);
    return it == realm->embedder_data.end() ? nullptr : it->second;
}

// ---- Exposing a class to script -------------------------------------------

ClassRef DefineClass(Context& ctx, const char* name, NativeFn constructor, int length, Object* parentProto) {
    RealmScope realm_scope(ctx.realm());
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

// The realm a class's members belong to: the one that made its constructor, which is
// the holder itself for a static member and the prototype's "constructor" for the rest.
// The members' natives are made while it is current, so they run in it.
static Quanta::Realm* realm_of_holder(Object* holder) {
    Object* fn = holder;
    if (holder->get_type() != Object::ObjectType::Function) {
        Value ctor = holder->get_property("constructor");
        fn = ctor.is_function() ? static_cast<Object*>(ctor.as_function()) : nullptr;
    }
    Context* home = fn ? Engine::find_realm_owning_function(fn) : nullptr;
    return home ? home->realm() : g_current_realm;
}

void DefineMethod(Object* proto, const char* name, NativeFn fn, int length) {
    RealmScope realm_scope(realm_of_holder(proto));
    proto->set_property(name, Value(make_native(name, fn, length).release()), PropertyAttributes::Default);
}

void DefineStaticMethod(Object* constructor, const char* name, NativeFn fn, int length) {
    RealmScope realm_scope(realm_of_holder(constructor));
    constructor->set_property(name, Value(make_native(name, fn, length).release()), PropertyAttributes::Default);
}

void DefineAccessor(Object* proto, const char* name, NativeFn getter, NativeFn setter) {
    RealmScope realm_scope(realm_of_holder(proto));
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
    RealmScope realm_scope(ctx.realm());
    ctx.register_built_in_object(name, constructor);
}

void DefineGlobalFunction(Context& ctx, const char* name, NativeFn fn, int length) {
    RealmScope realm_scope(ctx.realm());
    ctx.register_built_in_object(name, make_native(name, fn, length).release());
}

Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget) {
    Object* target = newTarget.as_object_or_null();
    if (!target) return nullptr;
    Value proto = target->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    return proto.as_object_or_null();
}

// GetFunctionRealm (7.3.24): through a Proxy's target and a bound function's, to the realm the
// function was made in.
static Quanta::Realm* function_realm(Context& ctx, Object* fn) {
    for (int depth = 0; fn && depth < 1000; depth++) {
        if (fn->get_type() == Object::ObjectType::Proxy) {
            Proxy* proxy = static_cast<Proxy*>(fn);
            if (proxy->is_revoked()) {
                ctx.throw_type_error("Cannot perform 'GetFunctionRealm' on a proxy that has been revoked");
                return nullptr;
            }
            fn = proxy->get_proxy_target();
            continue;
        }
        if (fn->get_type() != Object::ObjectType::Function) return nullptr;
        if (fn->has_internal_slot("__bound_target__")) {
            Value target = fn->get_internal_slot("__bound_target__");
            fn = target.as_object_or_null();
            continue;
        }
        Context* home = static_cast<Function*>(fn)->get_closure_context();
        return home ? home->realm() : nullptr;
    }
    return nullptr;
}

Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget, const void* fallbackKey) {
    Object* target = newTarget.as_object_or_null();
    if (!target) return nullptr;
    Value proto = target->get_property("prototype");
    if (ctx.has_exception()) return nullptr;
    if (Object* p = proto.as_object_or_null()) return p;
    Quanta::Realm* realm = function_realm(ctx, target);
    if (ctx.has_exception()) return nullptr;
    if (!realm) realm = ctx.realm();
    if (!realm) return nullptr;
    auto it = realm->embedder_data.find(fallbackKey);
    return it != realm->embedder_data.end() ? static_cast<Object*>(it->second) : nullptr;
}

Realm* GetFunctionRealm(Context& ctx, const Value& function) {
    Quanta::Realm* realm = function_realm(ctx, function.as_object_or_null());
    Engine* engine = realm ? realm->engine() : nullptr;
    return engine ? static_cast<Realm*>(engine->host_realm()) : nullptr;
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

// ---- Values, conversions and objects ---------------------------------------

bool ToBoolean(const Value& v) { return v.to_boolean(); }

double ToNumber(Context& ctx, const Value& v) {
    double d = std::nan("");
    v.to_number_checked(ctx, d);
    return d;
}

Value ToString(Context& ctx, const Value& v) {
    std::string s;
    if (!v.to_string_checked(ctx, s)) return Value();
    return Value(std::move(s));
}

Value ToPropertyKey(Context& ctx, const Value& v) {
    if (v.is_symbol()) return v;
    if (v.is_string()) return v;
    return ToString(ctx, v);
}

bool SameValue(const Value& a, const Value& b) { return a.same_value(b); }

bool InstanceOf(Context& ctx, const Value& v, const Value& ctor) {
    RealmScope realm_scope(ctx.realm());
    Object* rhs = ctor.as_object_or_null();
    if (!rhs) {
        ctx.throw_type_error("Right-hand side of 'instanceof' is not an object");
        return false;
    }
    Value has_instance = rhs->get_property("Symbol.hasInstance");
    if (ctx.has_exception()) return false;
    if (has_instance.is_function()) {
        Value args[1] = {v};
        Value result = has_instance.as_function()->call(ctx, std::vector<Value>(args, args + 1), ctor);
        return !ctx.has_exception() && result.to_boolean();
    }
    if (!ctor.is_function()) {
        ctx.throw_type_error("Right-hand side of 'instanceof' is not callable");
        return false;
    }
    bool result = ordinary_has_instance(ctx, ctor, v);
    return !ctx.has_exception() && result;
}

bool IsArray(Context& ctx, const Value& v) {
    Object* o = v.as_object_or_null();
    for (int depth = 0; o && depth < 1000; depth++) {
        if (o->get_type() != Object::ObjectType::Proxy) return o->is_array();
        Proxy* proxy = static_cast<Proxy*>(o);
        if (proxy->is_revoked()) {
            ctx.throw_type_error("Cannot perform 'IsArray' on a proxy that has been revoked");
            return false;
        }
        o = proxy->get_proxy_target();
    }
    return false;
}

bool IsConstructor(const Value& v) {
    Object* o = v.as_object_or_null();
    for (int depth = 0; o && depth < 1000; depth++) {
        if (o->get_type() == Object::ObjectType::Proxy) {
            o = static_cast<Proxy*>(o)->get_proxy_target();
            continue;
        }
        return o->get_type() == Object::ObjectType::Function && static_cast<Function*>(o)->is_constructor();
    }
    return false;
}

Value NewObject(Context& ctx) {
    RealmScope realm_scope(ctx.realm());
    return Value(ObjectFactory::create_object().release());
}

Value NewArray(Context& ctx, Args elements) {
    RealmScope realm_scope(ctx.realm());
    Value array(ObjectFactory::create_array(0).release());
    for (const Value& element : elements) array.as_object()->push(element);
    return array;
}

Value NewString(Context&, std::string_view utf8) {
    return Value(to_scalar_values(utf8, /*wtf8=*/false));
}

Value NewString(Context& ctx, std::u16string_view utf16) {
    return FromUtf16(ctx, utf16);
}

Value Construct(Context& ctx, const Value& ctor, Args args, const Value& newTarget) {
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    if (!IsConstructor(ctor)) {
        ctx.throw_type_error("Value is not a constructor");
        return Value();
    }
    const Value& target = newTarget.is_undefined() ? ctor : newTarget;
    if (!IsConstructor(target)) {
        ctx.throw_type_error("newTarget is not a constructor");
        return Value();
    }
    Value argv[3] = {ctor, NewArray(ctx, args), target};
    return Reflect::reflect_construct(ctx, std::span<const Value>(argv, 3), Value());
}

Value NewFunction(Context& ctx, std::string_view name, int length, NativeClosure closure) {
    RealmScope realm_scope(ctx.realm());
    auto fn = ObjectFactory::create_native_function_with_new_target(std::string(name),
        [closure = std::move(closure)](Context& c, std::span<const Value> args, Value receiver,
                                        bool is_construct, Value new_target) -> Value {
            if (is_construct) {
                c.throw_type_error("Function is not a constructor");
                return Value();
            }
            return closure(c, receiver, args, Value());
        },
        static_cast<uint32_t>(length));
    return Value(fn.release());
}

// The internal methods go through the Reflect functions, which dispatch on the kind of object
// (a Proxy's trap, a legacy platform object's hooks) as the spec's [[...]] methods do.
static bool require_object(Context& ctx, const Value& object) {
    if (object.as_object_or_null()) return true;
    ctx.throw_type_error("Value is not an object");
    return false;
}

Value GetIndex(Context& ctx, const Value& object, uint32_t index) {
    return Get(ctx, object, std::to_string(index));
}

bool HasProperty(Context& ctx, const Value& object, const Value& key) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value args[2] = {object, key};
    Value result = Reflect::reflect_has(ctx, std::span<const Value>(args, 2), Value());
    return !ctx.has_exception() && result.to_boolean();
}

bool HasProperty(Context& ctx, const Value& object, std::string_view name) {
    return HasProperty(ctx, object, Value(std::string(name)));
}

bool DeleteProperty(Context& ctx, const Value& object, const Value& key) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value args[2] = {object, key};
    Value result = Reflect::reflect_delete_property(ctx, std::span<const Value>(args, 2), Value());
    return !ctx.has_exception() && result.to_boolean();
}

bool DeleteProperty(Context& ctx, const Value& object, std::string_view name) {
    return DeleteProperty(ctx, object, Value(std::string(name)));
}

std::optional<Descriptor> GetOwnProperty(Context& ctx, const Value& object, const Value& key) {
    if (!require_object(ctx, object)) return std::nullopt;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value args[2] = {object, key};
    Value result = Reflect::reflect_get_own_property_descriptor(ctx, std::span<const Value>(args, 2), Value());
    if (ctx.has_exception()) return std::nullopt;
    Object* d = result.as_object_or_null();
    if (!d) return std::nullopt;
    Descriptor out;
    auto field = [&](const char* name, bool& has, Value& slot) {
        if ((has = d->has_own_property(name))) slot = d->get_property(name);
    };
    auto flag = [&](const char* name, bool& has, bool& slot) {
        if ((has = d->has_own_property(name))) slot = d->get_property(name).to_boolean();
    };
    field("value", out.has_value, out.value);
    field("get", out.has_get, out.get);
    field("set", out.has_set, out.set);
    flag("writable", out.has_writable, out.writable);
    flag("enumerable", out.has_enumerable, out.enumerable);
    flag("configurable", out.has_configurable, out.configurable);
    return out;
}

std::optional<Descriptor> GetOwnProperty(Context& ctx, const Value& object, std::string_view name) {
    return GetOwnProperty(ctx, object, Value(std::string(name)));
}

bool DefineProperty(Context& ctx, const Value& object, const Value& key, const Descriptor& descriptor) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value desc = NewObject(ctx);
    Object* d = desc.as_object();
    if (descriptor.has_value) d->set_property("value", descriptor.value);
    if (descriptor.has_get) d->set_property("get", descriptor.get);
    if (descriptor.has_set) d->set_property("set", descriptor.set);
    if (descriptor.has_writable) d->set_property("writable", Value(descriptor.writable));
    if (descriptor.has_enumerable) d->set_property("enumerable", Value(descriptor.enumerable));
    if (descriptor.has_configurable) d->set_property("configurable", Value(descriptor.configurable));
    Value args[3] = {object, key, desc};
    Value result = Reflect::reflect_define_property(ctx, std::span<const Value>(args, 3), Value());
    return !ctx.has_exception() && result.to_boolean();
}

bool DefineProperty(Context& ctx, const Value& object, std::string_view name, const Descriptor& descriptor) {
    return DefineProperty(ctx, object, Value(std::string(name)), descriptor);
}

Value GetPrototypeOf(Context& ctx, const Value& object) {
    if (!require_object(ctx, object)) return Value();
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    return Reflect::reflect_get_prototype_of(ctx, std::span<const Value>(&object, 1), Value());
}

bool SetPrototypeOf(Context& ctx, const Value& object, const Value& prototype) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value args[2] = {object, prototype};
    Value result = Reflect::reflect_set_prototype_of(ctx, std::span<const Value>(args, 2), Value());
    return !ctx.has_exception() && result.to_boolean();
}

bool IsExtensible(Context& ctx, const Value& object) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value result = Reflect::reflect_is_extensible(ctx, std::span<const Value>(&object, 1), Value());
    return !ctx.has_exception() && result.to_boolean();
}

bool PreventExtensions(Context& ctx, const Value& object) {
    if (!require_object(ctx, object)) return false;
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
    Value result = Reflect::reflect_prevent_extensions(ctx, std::span<const Value>(&object, 1), Value());
    return !ctx.has_exception() && result.to_boolean();
}

// ---- Keeping values alive ---------------------------------------------------

struct Persistent::Slot {
    std::vector<Value> values;
    ValueVectorRoot root;
    explicit Slot(const Value& value) : values{value}, root(&values) {}
};

Persistent::Persistent(Context&, const Value& value) : slot_(std::make_unique<Slot>(value)) {}
Persistent::Persistent() = default;
Persistent::~Persistent() = default;
Persistent::Persistent(Persistent&&) noexcept = default;
Persistent& Persistent::operator=(Persistent&&) noexcept = default;

Value Persistent::Get() const { return slot_ ? slot_->values[0] : Value(); }
void Persistent::Reset() { slot_.reset(); }

struct ValueList::Impl {
    std::vector<Value> values;
    ValueVectorRoot root;
    Impl() : root(&values) {}
};

ValueList::ValueList() : impl_(std::make_unique<Impl>()) {}
ValueList::~ValueList() = default;
ValueList::ValueList(ValueList&&) noexcept = default;
ValueList& ValueList::operator=(ValueList&&) noexcept = default;
size_t ValueList::size() const { return impl_->values.size(); }
const Value& ValueList::operator[](size_t index) const { return impl_->values[index]; }
const Value* ValueList::begin() const { return impl_->values.data(); }
const Value* ValueList::end() const { return impl_->values.data() + impl_->values.size(); }
void ValueList::Append(const Value& value) { impl_->values.push_back(value); }

// ---- Byte buffers ---------------------------------------------------------

Value NewUint8Array(Context& ctx, std::span<const uint8_t> bytes) {
    RealmScope realm_scope(ctx.realm());
    Object* ctor_object = ctx.get_built_in_object("Uint8Array");
    Function* ctor = ctor_object && ctor_object->get_type() == Object::ObjectType::Function
                         ? static_cast<Function*>(ctor_object) : ctx.get_built_in_function("Uint8Array");
    if (!ctor) {
        ctx.throw_type_error("Uint8Array is not available");
        return Value();
    }
    Value array = ctor->construct(ctx, std::vector<Value>{Value(static_cast<double>(bytes.size()))});
    if (ctx.has_exception()) return Value();
    if (!bytes.empty()) {
        auto view = BytesOf(array);
        if (!view) return Value();
        std::memcpy(const_cast<uint8_t*>(view->data()), bytes.data(), bytes.size());
    }
    return array;
}

std::optional<std::span<uint8_t>> MutableBytesOf(const Value& value) {
    Object* object = value.as_object_or_null();
    if (!object) return std::nullopt;
    if (object->is_array_buffer()) {
        ArrayBuffer* buffer = static_cast<ArrayBuffer*>(object);
        if (buffer->is_detached()) return std::nullopt;
        return std::span<uint8_t>(buffer->data(), buffer->byte_length());
    }
    ArrayBuffer* buffer = nullptr;
    size_t offset = 0, length = 0;
    if (object->is_typed_array()) {
        TypedArrayBase* view = static_cast<TypedArrayBase*>(object);
        if (view->is_out_of_bounds()) return std::nullopt;
        buffer = view->buffer();
        offset = view->byte_offset();
        length = view->byte_length();
    } else if (object->is_data_view()) {
        DataView* view = static_cast<DataView*>(object);
        if (view->is_out_of_bounds()) return std::nullopt;
        buffer = view->buffer();
        offset = view->byte_offset();
        length = view->current_byte_length();
    } else {
        return std::nullopt;
    }
    if (!buffer || buffer->is_detached()) return std::nullopt;
    return std::span<uint8_t>(buffer->data() + offset, length);
}

std::optional<std::span<const uint8_t>> BytesOf(const Value& value) {
    if (auto bytes = MutableBytesOf(value)) return std::span<const uint8_t>(bytes->data(), bytes->size());
    return std::nullopt;
}

Value NewArrayBuffer(Context& ctx, void* data, size_t size, void (*release)(void*, void*), void* user) {
    RealmScope realm_scope(ctx.realm());
    if (!data && size > 0) {
        ctx.throw_type_error("NewArrayBuffer: no data for a non-empty buffer");
        return Value();
    }
    auto store = std::make_shared<ArrayBuffer::BackingStore>();
    store->data = static_cast<uint8_t*>(data);
    store->byte_length.store(size, std::memory_order_relaxed);
    store->max_byte_length = size;
    store->external_release = release;
    store->external_user = user;
    ArrayBuffer* buffer = new ArrayBuffer(std::move(store));
    if (Object* proto = current_realm().array_buffer_proto) buffer->initialize_prototype(proto);
    if (size > 0) Heap::note_offheap_bytes(size);
    return Value(static_cast<Object*>(buffer));
}

static Value construct_builtin(Context& ctx, const char* name, std::vector<Value> args) {
    RealmScope realm_scope(ctx.realm());
    Object* ctor = ctx.get_built_in_object(name);
    if (!ctor || ctor->get_type() != Object::ObjectType::Function) {
        ctx.throw_type_error(std::string(name) + " is not available");
        return Value();
    }
    return Construct(ctx, Value(static_cast<Function*>(ctor)), Args(args.data(), args.size()));
}

Value NewArrayBuffer(Context& ctx, size_t size) {
    Value buffer = construct_builtin(ctx, "ArrayBuffer", {Value(static_cast<double>(size))});
    if (!ctx.has_exception() && size > 0) Heap::note_offheap_bytes(size);
    return buffer;
}

Value NewSharedArrayBuffer(Context& ctx, size_t size) {
    return construct_builtin(ctx, "SharedArrayBuffer", {Value(static_cast<double>(size))});
}

Value NewUint8Array(Context& ctx, const Value& buffer, size_t offset, size_t length) {
    return construct_builtin(ctx, "Uint8Array",
        {buffer, Value(static_cast<double>(offset)), Value(static_cast<double>(length))});
}

bool IsDetached(const Value& v) {
    Object* object = v.as_object_or_null();
    if (!object) return false;
    if (object->is_array_buffer()) return static_cast<ArrayBuffer*>(object)->is_detached();
    if (object->is_typed_array()) {
        ArrayBuffer* buffer = static_cast<TypedArrayBase*>(object)->buffer();
        return buffer && buffer->is_detached();
    }
    if (object->is_data_view()) {
        ArrayBuffer* buffer = static_cast<DataView*>(object)->buffer();
        return buffer && buffer->is_detached();
    }
    return false;
}

bool IsSharedArrayBuffer(const Value& v) {
    Object* object = v.as_object_or_null();
    return object && object->is_array_buffer() && object->is_shared_array_buffer();
}

bool DetachArrayBuffer(Context& ctx, const Value& v) {
    Object* object = v.as_object_or_null();
    if (!object || !object->is_array_buffer() || object->is_shared_array_buffer()) {
        ctx.throw_type_error("DetachArrayBuffer: not an ArrayBuffer");
        return false;
    }
    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(object);
    if (buffer->is_immutable()) {
        ctx.throw_type_error("DetachArrayBuffer: the buffer is immutable");
        return false;
    }
    buffer->detach();
    return true;
}

Value TransferArrayBuffer(Context& ctx, const Value& buffer) {
    Value transfer = Get(ctx, buffer, "transfer");
    if (ctx.has_exception()) return Value();
    return Call(ctx, transfer, buffer);
}

Value FromWtf8(Context&, std::string_view wtf8) {
    return Value(to_scalar_values(wtf8, /*wtf8=*/true, Surrogates::Keep));
}

Value FromUtf16(Context&, std::u16string_view units) {
    std::string out;
    out.reserve(units.size());
    for (size_t i = 0; i < units.size(); i++) {
        uint32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            i++;
        } else {
            append_utf8(out, unit);
        }
    }
    return Value(std::move(out));
}

std::string ToWtf8(Context& ctx, const Value& v) {
    std::string s;
    if (!v.to_string_checked(ctx, s)) return {};
    return s;
}

std::u16string ToUtf16(Context& ctx, const Value& v) {
    std::string s;
    if (!v.to_string_checked(ctx, s)) return {};
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        uint32_t cp = kReplacement;
        size_t length = 1;
        if (b < 0x80) {
            cp = b;
        } else if (b >= 0xC2 && b <= 0xDF && i + 1 < s.size()) {
            cp = ((b & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            length = 2;
        } else if (b >= 0xE0 && b <= 0xEF && i + 2 < s.size()) {
            cp = ((b & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
            length = 3;
        } else if (b >= 0xF0 && b <= 0xF4 && i + 3 < s.size()) {
            cp = ((b & 0x07u) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<uint8_t>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[i + 3]) & 0x3Fu);
            length = 4;
        }
        i += length;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

// ---- Arrays and properties ------------------------------------------------

Value NewArray(Context& ctx) {
    RealmScope realm_scope(ctx.realm());
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

ValueList OwnPropertyKeys(Context& ctx, const Value& object) {
    ValueList keys;
    if (!object.as_object_or_null()) {
        ctx.throw_type_error("Cannot enumerate a non-object");
        return keys;
    }
    RealmScope realm_scope(ctx.realm());
    Value array = Reflect::reflect_own_keys(ctx, std::span<const Value>(&object, 1), Value());
    if (ctx.has_exception()) return keys;
    Object* list = array.as_object_or_null();
    if (!list) return keys;
    Value length = list->get_property("length");
    for (uint32_t i = 0; i < static_cast<uint32_t>(length.to_number()); i++) {
        keys.Append(list->get_property(std::to_string(i)));
    }
    return keys;
}

bool GetOwnEnumerable(Context& ctx, const Value& object, const Value& key) {
    if (!object.as_object_or_null()) {
        ctx.throw_type_error("Cannot read properties of a non-object");
        return false;
    }
    RealmScope realm_scope(ctx.realm());
    Value args[2] = {object, key};
    Value desc = Reflect::reflect_get_own_property_descriptor(ctx, std::span<const Value>(args, 2), Value());
    if (ctx.has_exception()) return false;
    Object* descriptor = desc.as_object_or_null();
    if (!descriptor) return false;
    return descriptor->get_property("enumerable").to_boolean();
}

Value Get(Context& ctx, const Value& object, const Value& key) {
    if (!object.as_object_or_null()) {
        ctx.throw_type_error("Cannot read properties of a non-object");
        return Value();
    }
    RealmScope realm_scope(ctx.realm());
    Value args[2] = {object, key};
    return Reflect::reflect_get(ctx, std::span<const Value>(args, 2), Value());
}

// ---- Calling back into script ---------------------------------------------

Value Call(Context& ctx, const Value& callable, const Value& thisValue, Args args) {
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
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
    RealmScope realm_scope(ctx.realm());
    return Iterator::create_iterator_result(value, done, &ctx);
}

// ---- Promises -------------------------------------------------------------

PromiseCapability NewPromiseCapability(Context& ctx) {
    RealmScope realm_scope(ctx.realm());
    RunningContext running(ctx);
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

void ThrowRangeError(Context& ctx, const std::string& message) { ctx.throw_range_error(message); }
void ThrowSyntaxError(Context& ctx, const std::string& message) { ctx.throw_syntax_error(message); }
void ThrowReferenceError(Context& ctx, const std::string& message) { ctx.throw_reference_error(message); }

Value NewError(Context& ctx, std::string_view kind, std::string_view message) {
    Object* ctor = ctx.get_built_in_object(std::string(kind));
    if (!ctor) {
        // A class script defined, or a host bound with DefineGlobal and script then replaced.
        Object* global = ctx.get_global_object();
        ctor = global ? global->get_property(std::string(kind)).as_object_or_null() : nullptr;
    }
    if (!ctor || ctor->get_type() != Object::ObjectType::Function) {
        ctx.throw_type_error(std::string(kind) + " is not an error class");
        return Value();
    }
    Value text{std::string(message)};
    return Construct(ctx, Value(static_cast<Function*>(ctor)), Args(&text, 1));
}

void ThrowError(Context& ctx, std::string_view kind, std::string_view message) {
    Value error = NewError(ctx, kind, message);
    if (!ctx.has_exception()) Throw(ctx, error);
}

void Throw(Context& ctx, const Value& exception) {
    ctx.throw_exception(exception, /*raw=*/true);
}

}
