/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The HTML structured clone algorithm: StructuredSerializeInternal and StructuredDeserialize, over a byte
// stream that holds no pointer into any heap. Object identity is the order in which objects are first met,
// which both directions walk the same way, so a reference is just that number.

#include "quanta/core/engine/StructuredClone.h"
#include "quanta/Embed.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/TypedArray.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace Quanta {

namespace {

namespace E = Embed;

enum class Tag : uint8_t {
    Undefined, Null, True, False, Number, BigInt, String,
    ObjectRef,
    BooleanObject, NumberObject, BigIntObject, StringObject,
    Date, RegExp,
    ArrayBuffer, ResizableArrayBuffer, SharedArrayBuffer, TransferredArrayBuffer,
    TypedArray, DataView,
    Map, Set, Error, Array, Object, Host, TransferredHost,
    Property, End
};

// Index = TypedArrayBase::ArrayType.
constexpr const char* kTypedArrayNames[] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array",
    "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"};
constexpr const char* kErrorNames[] = {"Error", "EvalError", "RangeError", "ReferenceError",
                                       "SyntaxError", "TypeError", "URIError"};

constexpr int kMaxDepth = 2500;

class RunningScope {
public:
    explicit RunningScope(Context& ctx) : realm_(ctx.realm()), previous_(Object::current_context_) {
        Object::current_context_ = &ctx;
    }
    ~RunningScope() { Object::current_context_ = previous_; }
    RunningScope(const RunningScope&) = delete;
    RunningScope& operator=(const RunningScope&) = delete;

private:
    RealmScope realm_;
    Context* previous_;
};

void throw_data_clone_error(Context& ctx, const std::string& message) {
    Value dom = E::Get(ctx, Value(ctx.get_global_object()), "DOMException");
    if (ctx.has_exception()) return;
    if (dom.is_function()) {
        Value args[] = {Value(message), Value(std::string("DataCloneError"))};
        Value error = E::Construct(ctx, dom, E::Args(args, 2));
        if (!ctx.has_exception()) E::Throw(ctx, error);
        return;
    }
    Value error = E::NewError(ctx, "Error", message);
    if (ctx.has_exception()) return;
    E::Descriptor name;
    name.value = Value(std::string("DataCloneError"));
    name.has_value = name.has_writable = name.has_configurable = true;
    name.writable = name.configurable = true;
    E::DefineProperty(ctx, error, "name", name);
    E::Throw(ctx, error);
}

// A canonical array index: digits without a leading zero, below 2^32 - 1.
bool parse_index(const std::string& key, uint32_t& index) {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return false;
    uint64_t n = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    if (n >= 0xFFFFFFFFull) return false;
    index = static_cast<uint32_t>(n);
    return true;
}

Value builtin(Context& ctx, const char* name) {
    Object* ctor = ctx.get_built_in_object(name);
    if (!ctor || ctor->get_type() != Object::ObjectType::Function) return Value();
    return Value(static_cast<Function*>(ctor));
}

Value value_of(Object* o) {
    if (!o) return Value::null();
    if (o->get_type() == Object::ObjectType::Function) return Value(static_cast<Function*>(o));
    return Value(o);
}

// ---- The byte stream ------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
    void tag(Tag t) { out_.push_back(static_cast<uint8_t>(t)); }
    void u8(uint8_t v) { out_.push_back(v); }
    void varint(uint64_t v) {
        while (v >= 0x80) { out_.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
        out_.push_back(static_cast<uint8_t>(v));
    }
    void f64(double d) {
        uint8_t raw[8];
        std::memcpy(raw, &d, 8);
        out_.insert(out_.end(), raw, raw + 8);
    }
    void raw(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + size);
    }
    void str(const std::string& s) { varint(s.size()); raw(s.data(), s.size()); }

private:
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool ok() const { return ok_; }
    bool at_end() const { return p_ == end_; }
    uint8_t u8() { if (p_ == end_) { ok_ = false; return 0; } return *p_++; }
    Tag tag() { return static_cast<Tag>(u8()); }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t b = u8();
            if (!ok_) return 0;
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }
    double f64() {
        if (static_cast<size_t>(end_ - p_) < 8) { ok_ = false; return 0; }
        double d;
        std::memcpy(&d, p_, 8);
        p_ += 8;
        return d;
    }
    const uint8_t* raw(size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) { ok_ = false; return nullptr; }
        const uint8_t* at = p_;
        p_ += n;
        return at;
    }
    std::string str() {
        size_t n = varint();
        const uint8_t* at = raw(n);
        return at ? std::string(reinterpret_cast<const char*>(at), n) : std::string();
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

// ---- Serializing ----------------------------------------------------------------

class Serializer {
public:
    Serializer(Context& ctx, const SerializeOptions& options, SerializedData& out)
        : ctx_(ctx), options_(options), out_(out), w_(out.bytes),
          host_(ctx.get_engine() ? ctx.get_engine()->serialization_host() : nullptr) {}

    bool run(const Value& value) {
        out_.for_storage = options_.for_storage;
        if (!register_transfers()) return false;
        if (!serialize(value)) return false;
        return finish_transfers();
    }

private:
    bool fail(const std::string& message) {
        if (!ctx_.has_exception()) throw_data_clone_error(ctx_, message);
        return false;
    }

    SerializationHost::Mode mode(bool transferring) const {
        SerializationHost::Mode m;
        m.transferring = transferring;
        m.for_storage = options_.for_storage;
        return m;
    }

    bool register_transfers() {
        for (size_t i = 0; i < options_.transfer.size(); i++) {
            Object* object = options_.transfer[i].as_object_or_null();
            if (!object) return fail("Value not transferable");
            if (transfer_slot_.count(object)) return fail("Transfer list contains the same object twice");
            if (object->is_array_buffer()) {
                ArrayBuffer* buffer = static_cast<ArrayBuffer*>(object);
                if (buffer->is_shared()) return fail("A SharedArrayBuffer cannot be transferred");
                if (buffer->is_detached()) return fail("An ArrayBuffer is detached and could not be cloned");
                if (buffer->is_immutable()) return fail("An immutable ArrayBuffer cannot be transferred");
            } else if (!(host_ && host_->is_transferable(ctx_, object))) {
                return fail("Value not transferable");
            }
            transfer_slot_[object] = i;
        }
        return true;
    }

    // The serialization is done: now the transferred objects give up their state.
    bool finish_transfers() {
        for (const Value& v : options_.transfer) {
            Object* object = v.as_object_or_null();
            SerializedData::Transfer slot;
            if (object->is_array_buffer()) {
                ArrayBuffer* buffer = static_cast<ArrayBuffer*>(object);
                slot.kind = SerializedData::Transfer::Kind::ArrayBuffer;
                slot.index = out_.stores.size();
                out_.stores.push_back(buffer->backing_store());
                buffer->detach();
            } else {
                HostObjectData data;
                std::string error;
                if (!host_->serialize(ctx_, object, mode(true), data, error)) return fail(error.empty() ? "Value not transferable" : error);
                if (!data.values.empty()) return fail("A transferred host object cannot carry values");
                slot.kind = SerializedData::Transfer::Kind::Host;
                slot.index = out_.host_transfers.size();
                out_.host_transfers.push_back({std::move(data.tag), std::move(data.bytes)});
            }
            out_.transfers.push_back(slot);
        }
        return true;
    }

    // An object met for the first time gets the next number; the collector must not free it (and let another
    // take its address) while the walk is under way.
    void remember(Object* object) {
        memory_[object] = next_id_++;
        roots_.Append(value_of(object));
    }

    bool serialize(const Value& value) {
        if (ctx_.has_exception()) return false;
        if (value.is_undefined()) { w_.tag(Tag::Undefined); return true; }
        if (value.is_null()) { w_.tag(Tag::Null); return true; }
        if (value.is_boolean()) { w_.tag(value.as_boolean() ? Tag::True : Tag::False); return true; }
        if (value.is_number()) { w_.tag(Tag::Number); w_.f64(value.as_number()); return true; }
        if (value.is_bigint()) { w_.tag(Tag::BigInt); w_.str(value.as_bigint()->to_string()); return true; }
        if (value.is_string()) { w_.tag(Tag::String); w_.str(value.to_string()); return true; }
        if (value.is_symbol()) return fail("Symbol could not be cloned");
        Object* object = value.as_object_or_null();
        if (!object) return fail("The value could not be cloned");

        auto seen = memory_.find(object);
        if (seen != memory_.end()) { w_.tag(Tag::ObjectRef); w_.varint(seen->second); return true; }

        if (depth_ >= kMaxDepth) {
            E::ThrowRangeError(ctx_, "Maximum call stack size exceeded");
            return false;
        }
        depth_++;
        bool ok = serialize_object(object, value);
        depth_--;
        return ok;
    }

    bool serialize_object(Object* object, const Value& value) {
        using OT = Object::ObjectType;
        auto transfer = transfer_slot_.find(object);
        if (transfer != transfer_slot_.end()) {
            w_.tag(object->is_array_buffer() ? Tag::TransferredArrayBuffer : Tag::TransferredHost);
            w_.varint(transfer->second);
            remember(object);
            return true;
        }
        switch (object->get_type()) {
            case OT::Boolean: {
                remember(object);
                w_.tag(Tag::BooleanObject);
                w_.u8(object->get_property("[[PrimitiveValue]]").as_boolean() ? 1 : 0);
                return true;
            }
            case OT::Number: {
                remember(object);
                w_.tag(Tag::NumberObject);
                w_.f64(object->get_property("[[PrimitiveValue]]").to_number());
                return true;
            }
            case OT::BigInt: {
                remember(object);
                w_.tag(Tag::BigIntObject);
                w_.str(object->get_property("[[PrimitiveValue]]").as_bigint()->to_string());
                return true;
            }
            case OT::String: {
                remember(object);
                w_.tag(Tag::StringObject);
                w_.str(object->get_property("[[PrimitiveValue]]").to_string());
                return true;
            }
            case OT::Date: {
                remember(object);
                w_.tag(Tag::Date);
                w_.f64(object->get_internal_slot("[[DateValue]]").to_number());
                return true;
            }
            case OT::RegExp: {
                RegExpObject* re = RegExpObject::from(object);
                if (!re || !re->impl()) return fail("RegExp could not be cloned");
                remember(object);
                w_.tag(Tag::RegExp);
                w_.str(re->impl()->get_source());
                w_.str(re->impl()->get_flags());
                return true;
            }
            case OT::ArrayBuffer: return serialize_buffer(static_cast<ArrayBuffer*>(object));
            case OT::TypedArray: return serialize_typed_array(static_cast<TypedArrayBase*>(object));
            case OT::DataView: return serialize_data_view(static_cast<DataView*>(object));
            case OT::Map: return serialize_map(object);
            case OT::Set: return serialize_set(object);
            case OT::Error: return serialize_error(object, value);
            case OT::Array: return serialize_properties(object, value, Tag::Array);
            case OT::Ordinary: return serialize_properties(object, value, Tag::Object);
            case OT::Custom: {
                using CK = CustomObjectBase::CustomKind;
                if (static_cast<CustomObjectBase*>(object)->get_custom_kind() == CK::Host) return serialize_host(object);
                return fail("The object could not be cloned");
            }
            case OT::Function: return fail("A function could not be cloned");
            case OT::Proxy: return fail("A Proxy object could not be cloned");
            case OT::Symbol: return fail("Symbol object could not be cloned");
            default: return fail("The object could not be cloned");
        }
    }

    bool serialize_buffer(ArrayBuffer* buffer) {
        if (buffer->is_shared()) {
            if (options_.for_storage) return fail("A SharedArrayBuffer cannot be stored");
            remember(buffer);
            w_.tag(Tag::SharedArrayBuffer);
            w_.varint(out_.stores.size());
            out_.stores.push_back(buffer->backing_store());
            return true;
        }
        if (buffer->is_detached()) return fail("An ArrayBuffer is detached and could not be cloned");
        remember(buffer);
        if (buffer->is_resizable()) {
            w_.tag(Tag::ResizableArrayBuffer);
            w_.varint(buffer->max_byte_length());
        } else {
            w_.tag(Tag::ArrayBuffer);
        }
        w_.varint(buffer->byte_length());
        w_.raw(buffer->data(), buffer->byte_length());
        return true;
    }

    bool serialize_typed_array(TypedArrayBase* view) {
        if (view->is_out_of_bounds()) return fail("An ArrayBuffer is detached or out of bounds and could not be cloned");
        w_.tag(Tag::TypedArray);
        if (!serialize(value_of(view->buffer()))) return false;
        remember(view);
        w_.u8(static_cast<uint8_t>(view->get_array_type()));
        w_.varint(view->byte_offset());
        w_.u8(view->is_length_tracking() ? 1 : 0);
        w_.varint(view->length());
        return true;
    }

    bool serialize_data_view(DataView* view) {
        if (view->is_out_of_bounds()) return fail("An ArrayBuffer is detached or out of bounds and could not be cloned");
        w_.tag(Tag::DataView);
        if (!serialize(value_of(view->buffer()))) return false;
        remember(view);
        w_.varint(view->byte_offset());
        w_.u8(view->is_length_tracking() ? 1 : 0);
        w_.varint(view->current_byte_length());
        return true;
    }

    bool serialize_map(Object* map) {
        // The entries as they are now: script that runs while they are written cannot change what is cloned.
        E::ValueList entries = E::MapEntries(value_of(map));
        remember(map);
        w_.tag(Tag::Map);
        w_.varint(entries.size() / 2);
        for (size_t i = 0; i < entries.size(); i++) {
            if (!serialize(entries[i])) return false;
        }
        return true;
    }

    bool serialize_set(Object* set) {
        E::ValueList values = E::SetValues(value_of(set));
        remember(set);
        w_.tag(Tag::Set);
        w_.varint(values.size());
        for (size_t i = 0; i < values.size(); i++) {
            if (!serialize(values[i])) return false;
        }
        return true;
    }

    bool serialize_error(Object* object, const Value& value) {
        Value name_value = E::Get(ctx_, value, "name");
        if (ctx_.has_exception()) return false;
        std::string name = "Error";
        if (name_value.is_string()) {
            std::string given = name_value.to_string();
            for (const char* known : kErrorNames) if (given == known) { name = given; break; }
        }
        std::optional<E::Descriptor> message = E::GetOwnProperty(ctx_, value, "message");
        if (ctx_.has_exception()) return false;
        std::string message_text;
        bool has_message = false;
        if (message && message->has_value) {
            message_text = E::ToWtf8(ctx_, message->value);
            if (ctx_.has_exception()) return false;
            has_message = true;
        }
        std::optional<E::Descriptor> cause = E::GetOwnProperty(ctx_, value, "cause");
        if (ctx_.has_exception()) return false;
        Error* error = as_error(object);
        remember(object);
        w_.tag(Tag::Error);
        w_.str(name);
        w_.u8(has_message ? 1 : 0);
        if (has_message) w_.str(message_text);
        w_.str(error ? error->get_stack_trace() : std::string());
        const bool has_cause = cause && cause->has_value;
        w_.u8(has_cause ? 1 : 0);
        if (has_cause) return serialize(cause->value);
        return true;
    }

    bool serialize_properties(Object* object, const Value& value, Tag tag) {
        remember(object);
        w_.tag(tag);
        if (tag == Tag::Array) w_.varint(object->get_length());
        std::vector<std::string> keys = E::OwnKeys(ctx_, value);
        if (ctx_.has_exception()) return false;
        for (const std::string& key : keys) {
            // A getter that ran for an earlier key may have deleted this one.
            if (!object->has_own_property(key)) continue;
            Value property = E::Get(ctx_, value, E::FromWtf8(ctx_, key));
            if (ctx_.has_exception()) return false;
            w_.tag(Tag::Property);
            w_.str(key);
            if (!serialize(property)) return false;
        }
        w_.tag(Tag::End);
        return true;
    }

    bool serialize_host(Object* object) {
        if (!host_) return fail("The object could not be cloned");
        HostObjectData data;
        std::string error;
        if (!host_->serialize(ctx_, object, mode(false), data, error)) {
            return fail(error.empty() ? "The object could not be cloned" : error);
        }
        if (ctx_.has_exception()) return false;
        E::ValueList held;
        for (const Value& v : data.values) held.Append(v);
        remember(object);
        w_.tag(Tag::Host);
        w_.str(data.tag);
        w_.varint(data.bytes.size());
        w_.raw(data.bytes.data(), data.bytes.size());
        w_.varint(data.values.size());
        for (size_t i = 0; i < held.size(); i++) {
            if (!serialize(held[i])) return false;
        }
        return true;
    }

    Context& ctx_;
    const SerializeOptions& options_;
    SerializedData& out_;
    Writer w_;
    SerializationHost* host_;
    std::unordered_map<Object*, uint32_t> memory_;
    std::unordered_map<Object*, size_t> transfer_slot_;
    E::ValueList roots_;
    uint32_t next_id_ = 0;
    int depth_ = 0;
};

// ---- Deserializing --------------------------------------------------------------

class Deserializer {
public:
    Deserializer(Context& ctx, SerializedData& data)
        : ctx_(ctx), data_(data), r_(data.bytes.data(), data.bytes.size()),
          host_(ctx.get_engine() ? ctx.get_engine()->serialization_host() : nullptr) {}

    Value run() {
        Value result;
        if (!read(result)) return Value();
        if (!r_.ok() || !r_.at_end()) {
            corrupt();
            return Value();
        }
        return result;
    }

private:
    bool corrupt() {
        if (!ctx_.has_exception()) throw_data_clone_error(ctx_, "The serialized data is corrupt");
        return false;
    }

    bool read(Value& out) {
        if (ctx_.has_exception()) return false;
        Tag tag = r_.tag();
        if (!r_.ok()) return corrupt();
        switch (tag) {
            case Tag::Undefined: out = Value(); return true;
            case Tag::Null: out = Value::null(); return true;
            case Tag::True: out = Value(true); return true;
            case Tag::False: out = Value(false); return true;
            case Tag::Number: out = Value(r_.f64()); return r_.ok() || corrupt();
            case Tag::BigInt: out = make_bigint(r_.str()); return r_.ok() || corrupt();
            case Tag::String: out = Value(r_.str()); return r_.ok() || corrupt();
            case Tag::ObjectRef: {
                size_t id = r_.varint();
                if (!r_.ok() || id >= made_.size() || pending_.count(id)) return corrupt();
                out = made_[id];
                return true;
            }
            default: break;
        }
        if (depth_ >= kMaxDepth) {
            E::ThrowRangeError(ctx_, "Maximum call stack size exceeded");
            return false;
        }
        depth_++;
        bool ok = read_object(tag, out);
        depth_--;
        return ok;
    }

    static Value make_bigint(const std::string& digits) { return Value(new BigInt(BigInt::from_string(digits))); }

    // Registers a made object under the next number.
    Value adopt(const Value& v) {
        if (ctx_.has_exception()) return Value();
        made_.push_back(v);
        roots_.Append(v);
        return v;
    }

    bool read_object(Tag tag, Value& out) {
        switch (tag) {
            case Tag::BooleanObject: {
                bool b = r_.u8() != 0;
                out = adopt(box(Value(b)));
                return !ctx_.has_exception();
            }
            case Tag::NumberObject: {
                double d = r_.f64();
                out = adopt(box(Value(d)));
                return r_.ok() && !ctx_.has_exception() ? true : corrupt();
            }
            case Tag::BigIntObject: {
                std::string digits = r_.str();
                out = adopt(box(make_bigint(digits)));
                return r_.ok() && !ctx_.has_exception() ? true : corrupt();
            }
            case Tag::StringObject: {
                std::string s = r_.str();
                out = adopt(box(Value(s)));
                return r_.ok() && !ctx_.has_exception() ? true : corrupt();
            }
            case Tag::Date: {
                double t = r_.f64();
                Value args[] = {Value(t)};
                out = adopt(E::Construct(ctx_, builtin(ctx_, "Date"), E::Args(args, 1)));
                return r_.ok() && !ctx_.has_exception() ? true : corrupt();
            }
            case Tag::RegExp: {
                std::string source = r_.str();
                std::string flags = r_.str();
                Value args[] = {Value(source), Value(flags)};
                out = adopt(E::Construct(ctx_, builtin(ctx_, "RegExp"), E::Args(args, 2)));
                return r_.ok() && !ctx_.has_exception() ? true : corrupt();
            }
            case Tag::ArrayBuffer:
            case Tag::ResizableArrayBuffer: return read_buffer(tag == Tag::ResizableArrayBuffer, out);
            case Tag::SharedArrayBuffer: return read_shared_buffer(out);
            case Tag::TransferredArrayBuffer: return read_transferred_buffer(out);
            case Tag::TransferredHost: return read_transferred_host(out);
            case Tag::TypedArray: return read_typed_array(out);
            case Tag::DataView: return read_data_view(out);
            case Tag::Map: return read_map(out);
            case Tag::Set: return read_set(out);
            case Tag::Error: return read_error(out);
            case Tag::Array: return read_properties(true, out);
            case Tag::Object: return read_properties(false, out);
            case Tag::Host: return read_host(out);
            default: return corrupt();
        }
    }

    Value box(const Value& primitive) {
        Value object_ctor = builtin(ctx_, "Object");
        Value args[] = {primitive};
        return E::Call(ctx_, object_ctor, Value(), E::Args(args, 1));
    }

    bool read_buffer(bool resizable, Value& out) {
        size_t max_length = resizable ? r_.varint() : 0;
        size_t length = r_.varint();
        const uint8_t* bytes = r_.raw(length);
        if (!r_.ok()) return corrupt();
        Value buffer;
        if (resizable) {
            Value args[] = {Value(static_cast<double>(length)), Value()};
            Value options = E::NewObject(ctx_);
            E::Set(ctx_, options, "maxByteLength", Value(static_cast<double>(max_length)));
            args[1] = options;
            buffer = E::Construct(ctx_, builtin(ctx_, "ArrayBuffer"), E::Args(args, 2));
        } else {
            buffer = E::NewArrayBuffer(ctx_, length);
        }
        if (ctx_.has_exception()) return false;
        if (length > 0) std::memcpy(static_cast<ArrayBuffer*>(buffer.as_object())->data(), bytes, length);
        out = adopt(buffer);
        return true;
    }

    bool read_shared_buffer(Value& out) {
        size_t index = r_.varint();
        if (!r_.ok() || index >= data_.stores.size()) return corrupt();
        auto* sab = new SharedArrayBuffer(data_.stores[index]);
        Value ctor = builtin(ctx_, "SharedArrayBuffer");
        Value proto = ctor.is_function() ? ctor.as_function()->get_property("prototype") : Value();
        if (proto.is_object()) sab->initialize_prototype(proto.as_object());
        out = adopt(Value(static_cast<Object*>(sab)));
        return true;
    }

    bool read_transferred_buffer(Value& out) {
        size_t slot = r_.varint();
        if (!r_.ok() || slot >= data_.transfers.size()) return corrupt();
        SerializedData::Transfer& t = data_.transfers[slot];
        if (t.kind != SerializedData::Transfer::Kind::ArrayBuffer) return corrupt();
        if (t.consumed || !data_.stores[t.index]) {
            throw_data_clone_error(ctx_, "A transferred ArrayBuffer was already received");
            return false;
        }
        std::shared_ptr<ArrayBuffer::BackingStore> store = std::move(data_.stores[t.index]);
        t.consumed = true;
        auto* buffer = new ArrayBuffer(std::move(store));
        if (Object* proto = ctx_.realm() ? ctx_.realm()->array_buffer_proto : nullptr) buffer->initialize_prototype(proto);
        out = adopt(Value(static_cast<Object*>(buffer)));
        return true;
    }

    bool read_transferred_host(Value& out) {
        size_t slot = r_.varint();
        if (!r_.ok() || slot >= data_.transfers.size() || !host_) return corrupt();
        SerializedData::Transfer& t = data_.transfers[slot];
        if (t.kind != SerializedData::Transfer::Kind::Host) return corrupt();
        if (t.consumed) {
            throw_data_clone_error(ctx_, "A transferred object was already received");
            return false;
        }
        t.consumed = true;
        HostObjectData record;
        record.tag = data_.host_transfers[t.index].tag;
        record.bytes = data_.host_transfers[t.index].bytes;
        Value made = host_->deserialize(ctx_, record, true);
        if (ctx_.has_exception()) return false;
        out = adopt(made);
        return true;
    }

    bool read_typed_array(Value& out) {
        Value buffer;
        if (!read(buffer)) return false;
        uint8_t type = r_.u8();
        size_t offset = r_.varint();
        bool tracking = r_.u8() != 0;
        size_t length = r_.varint();
        if (!r_.ok() || type >= sizeof(kTypedArrayNames) / sizeof(*kTypedArrayNames) || !buffer.is_object()) return corrupt();
        Value args[] = {buffer, Value(static_cast<double>(offset)), Value(static_cast<double>(length))};
        Value view = E::Construct(ctx_, builtin(ctx_, kTypedArrayNames[type]), E::Args(args, tracking ? 2 : 3));
        if (ctx_.has_exception()) return false;
        out = adopt(view);
        return true;
    }

    bool read_data_view(Value& out) {
        Value buffer;
        if (!read(buffer)) return false;
        size_t offset = r_.varint();
        bool tracking = r_.u8() != 0;
        size_t length = r_.varint();
        if (!r_.ok() || !buffer.is_object()) return corrupt();
        Value args[] = {buffer, Value(static_cast<double>(offset)), Value(static_cast<double>(length))};
        Value view = E::Construct(ctx_, builtin(ctx_, "DataView"), E::Args(args, tracking ? 2 : 3));
        if (ctx_.has_exception()) return false;
        out = adopt(view);
        return true;
    }

    bool read_map(Value& out) {
        Value map = E::Construct(ctx_, builtin(ctx_, "Map"));
        if (ctx_.has_exception()) return false;
        out = adopt(map);
        size_t count = r_.varint();
        if (!r_.ok()) return corrupt();
        Map* internal = static_cast<Map*>(map.as_object());
        for (size_t i = 0; i < count; i++) {
            Value key, value;
            if (!read(key) || !read(value)) return false;
            internal->set(key, value);
        }
        return true;
    }

    bool read_set(Value& out) {
        Value set = E::Construct(ctx_, builtin(ctx_, "Set"));
        if (ctx_.has_exception()) return false;
        out = adopt(set);
        size_t count = r_.varint();
        if (!r_.ok()) return corrupt();
        Quanta::Set* internal = static_cast<Quanta::Set*>(set.as_object());
        for (size_t i = 0; i < count; i++) {
            Value element;
            if (!read(element)) return false;
            internal->add(element);
        }
        return true;
    }

    bool read_error(Value& out) {
        std::string name = r_.str();
        bool has_message = r_.u8() != 0;
        std::string message = has_message ? r_.str() : std::string();
        std::string stack = r_.str();
        bool has_cause = r_.u8() != 0;
        if (!r_.ok()) return corrupt();
        Value ctor = builtin(ctx_, name.c_str());
        if (!ctor.is_function()) ctor = builtin(ctx_, "Error");
        Value args[] = {Value(message)};
        Value error = E::Construct(ctx_, ctor, E::Args(args, has_message ? 1 : 0));
        if (ctx_.has_exception()) return false;
        if (Error* e = as_error(error.as_object())) e->set_stack_trace(stack);
        out = adopt(error);
        if (has_cause) {
            Value cause;
            if (!read(cause)) return false;
            E::Descriptor d;
            d.value = cause;
            d.has_value = d.has_writable = d.has_configurable = d.has_enumerable = true;
            d.writable = d.configurable = true;
            d.enumerable = false;
            E::DefineProperty(ctx_, error, "cause", d);
            if (ctx_.has_exception()) return false;
        }
        return true;
    }

    bool read_properties(bool is_array, Value& out) {
        Value object;
        size_t length = 0;
        if (is_array) {
            length = r_.varint();
            if (!r_.ok()) return corrupt();
            object = E::NewArray(ctx_);
            if (ctx_.has_exception()) return false;
            E::Set(ctx_, object, "length", Value(static_cast<double>(length)));
        } else {
            object = E::NewObject(ctx_);
        }
        if (ctx_.has_exception()) return false;
        out = adopt(object);
        while (true) {
            Tag next = r_.tag();
            if (!r_.ok()) return corrupt();
            if (next == Tag::End) return true;
            if (next != Tag::Property) return corrupt();
            std::string key = r_.str();
            Value value;
            if (!r_.ok() || !read(value)) return r_.ok() ? false : corrupt();
            // An element of a fresh array is a plain store; going through a descriptor would put each one in the
            // property table.
            uint32_t index;
            if (is_array && parse_index(key, index) && index < length) {
                object.as_object()->set_element(index, value);
                continue;
            }
            E::Descriptor d;
            d.value = value;
            d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
            d.writable = d.enumerable = d.configurable = true;
            E::DefineProperty(ctx_, object, E::FromWtf8(ctx_, key), d);
            if (ctx_.has_exception()) return false;
        }
    }

    bool read_host(Value& out) {
        if (!host_) return corrupt();
        HostObjectData record;
        record.tag = r_.str();
        size_t size = r_.varint();
        const uint8_t* bytes = r_.raw(size);
        size_t count = r_.varint();
        if (!r_.ok()) return corrupt();
        record.bytes.assign(bytes, bytes + size);
        // The object takes its number before its values do, but exists only after them: a reference to it from
        // inside them has nothing to point at yet.
        size_t slot = made_.size();
        made_.push_back(Value());
        pending_.insert(slot);
        E::ValueList values;
        for (size_t i = 0; i < count; i++) {
            Value v;
            if (!read(v)) return false;
            values.Append(v);
            record.values.push_back(v);
        }
        Value made = host_->deserialize(ctx_, record, false);
        if (ctx_.has_exception()) return false;
        made_[slot] = made;
        roots_.Append(made);
        pending_.erase(slot);
        out = made;
        return true;
    }

    Context& ctx_;
    SerializedData& data_;
    Reader r_;
    SerializationHost* host_;
    E::ValueList roots_;                   // keeps every object made so far alive
    std::vector<Value> made_;              // by number
    std::unordered_set<size_t> pending_;
    int depth_ = 0;
};

}

bool structured_serialize(Context& ctx, const Value& value, const SerializeOptions& options, SerializedData& out) {
    RunningScope running(ctx);
    Serializer serializer(ctx, options, out);
    return serializer.run(value);
}

Value structured_deserialize(Context& ctx, SerializedData& data) {
    RunningScope running(ctx);
    Deserializer deserializer(ctx, data);
    return deserializer.run();
}

Value structured_clone_global(Context& ctx, std::span<const Value> args) {
    if (args.empty()) {
        ctx.throw_type_error("structuredClone: 1 argument required, but only 0 present");
        return Value();
    }
    RunningScope running(ctx);
    SerializeOptions options;
    E::ValueList roots;
    Value dictionary = args.size() > 1 ? args[1] : Value();
    if (!dictionary.is_nullish()) {
        if (!dictionary.is_object_like()) {
            ctx.throw_type_error("structuredClone: the options must be an object");
            return Value();
        }
        Value transfer = E::Get(ctx, dictionary, "transfer");
        if (ctx.has_exception()) return Value();
        if (!transfer.is_undefined()) {
            Value method = transfer.is_nullish() ? Value() : E::GetIteratorMethod(ctx, transfer);
            if (ctx.has_exception()) return Value();
            if (!method.is_function()) {
                ctx.throw_type_error("structuredClone: transfer must be iterable");
                return Value();
            }
            Value iterator = E::Call(ctx, method, transfer);
            if (ctx.has_exception()) return Value();
            if (!iterator.is_object_like()) {
                ctx.throw_type_error("structuredClone: transfer is not iterable");
                return Value();
            }
            Value next = E::Get(ctx, iterator, "next");
            if (ctx.has_exception()) return Value();
            while (true) {
                Value step = E::Call(ctx, next, iterator);
                if (ctx.has_exception()) return Value();
                if (!step.is_object_like()) {
                    ctx.throw_type_error("structuredClone: iterator result is not an object");
                    return Value();
                }
                Value done = E::Get(ctx, step, "done");
                if (ctx.has_exception()) return Value();
                if (E::ToBoolean(done)) break;
                Value item = E::Get(ctx, step, "value");
                if (ctx.has_exception()) return Value();
                if (!item.is_object_like()) {
                    ctx.throw_type_error("structuredClone: transfer entries must be objects");
                    return Value();
                }
                roots.Append(item);
                options.transfer.push_back(item);
            }
        }
    }
    return structured_clone(ctx, args[0], options);
}

Value structured_clone(Context& ctx, const Value& value, const SerializeOptions& options) {
    SerializedData data;
    if (!structured_serialize(ctx, value, options, data)) return Value();
    return structured_deserialize(ctx, data);
}

}
