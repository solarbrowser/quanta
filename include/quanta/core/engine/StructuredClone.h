/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_STRUCTURED_CLONE_H
#define QUANTA_ENGINE_STRUCTURED_CLONE_H

#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/Value.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Quanta {

class Context;
class Object;

// A host (platform) object, flattened: what its serialization steps produce and its deserialization steps
// take. `values` are JS values it holds (an ImageData's pixel array, say), cloned along with it so that
// identity and cycles between them and the rest of the graph are kept; they must stay reachable from the
// object while it is serialized.
struct HostObjectData {
    std::string tag;
    std::vector<uint8_t> bytes;
    std::vector<Value> values;
};

// The places the structured clone algorithm hands a host object to its owner. With none set, a host
// object is a DataCloneError.
class SerializationHost {
public:
    virtual ~SerializationHost() = default;

    struct Mode {
        bool transferring = false;   // the object is in the transfer list: take its state out, leaving it unusable
        bool for_storage = false;    // history.state, IndexedDB: nothing that is shared may go
    };
    // False, with `error` the message of the DataCloneError, for an object that is not serializable.
    virtual bool serialize(Context& ctx, Object* object, const Mode& mode, HostObjectData& out, std::string& error) = 0;
    // Whether `object` may appear in a transfer list.
    virtual bool is_transferable(Context& ctx, Object* object) = 0;
    // The object a record stands for, in the realm of `ctx`; undefined with an exception pending when it cannot
    // be made.
    virtual Value deserialize(Context& ctx, const HostObjectData& data, bool transferred) = 0;
};

// A serialized value: plain bytes and buffer stores, holding no reference to any heap, so it can be handed to
// another Isolate on another thread and deserialized there. A SharedArrayBuffer in it is the same memory in
// both; a transferred ArrayBuffer is moved, and can be deserialized only once.
struct SerializedData {
    struct Transfer {
        enum class Kind { ArrayBuffer, Host } kind = Kind::ArrayBuffer;
        size_t index = 0;            // into `stores` or `host_transfers`
        bool consumed = false;
    };
    struct HostTransfer {
        std::string tag;
        std::vector<uint8_t> bytes;
    };

    std::vector<uint8_t> bytes;
    std::vector<std::shared_ptr<ArrayBuffer::BackingStore>> stores;
    std::vector<Transfer> transfers;
    std::vector<HostTransfer> host_transfers;
    bool for_storage = false;

    size_t byte_size() const { return bytes.size(); }
};

struct SerializeOptions {
    std::vector<Value> transfer;     // kept alive by the caller for the duration of the call
    bool for_storage = false;
};

// StructuredSerializeWithTransfer. False with an exception (a DataCloneError) pending.
bool structured_serialize(Context& ctx, const Value& value, const SerializeOptions& options, SerializedData& out);
// StructuredDeserializeWithTransfer, into the realm of `ctx`. Undefined with an exception pending on failure.
Value structured_deserialize(Context& ctx, SerializedData& data);
// structuredClone(value, { transfer }) on already converted arguments.
Value structured_clone(Context& ctx, const Value& value, const SerializeOptions& options);
// The global function itself: `structuredClone(value, options)` with its WebIDL argument handling.
Value structured_clone_global(Context& ctx, std::span<const Value> args);

}

#endif
