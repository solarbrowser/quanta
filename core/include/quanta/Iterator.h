/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Object.h"
#include <memory>
#include <vector>
#include <functional>

namespace Quanta {

class Context;
class Symbol;

/**
 * JavaScript Iterator protocol implementation
 * Implements the ES6 iteration protocol with Symbol.iterator
 */
class Iterator : public Object {
public:
    struct IteratorResult {
        Value value;
        bool done;
        
        IteratorResult(const Value& v, bool d) : value(v), done(d) {}
    };
    
    using NextFunction = std::function<IteratorResult()>;
    
private:
    NextFunction next_fn_;
    bool done_;
    
public:
    Iterator(NextFunction next_fn);
    Iterator();
    virtual ~Iterator() = default;
    
    void set_next_function(NextFunction next_fn);
    
    virtual IteratorResult next();
    
    static Value iterator_next(Context& ctx, const std::vector<Value>& args);
    static Value iterator_return(Context& ctx, const std::vector<Value>& args);
    static Value iterator_throw(Context& ctx, const std::vector<Value>& args);
    
    static void setup_iterator_prototype(Context& ctx);
    
    static Value create_iterator_result(const Value& value, bool done);
};

/**
 * Array Iterator implementation
 * Iterates over array elements
 */
class ArrayIterator : public Iterator {
public:
    enum class Kind {
        Keys,
        Values,
        Entries
    };
    
private:
    Object* array_;
    Kind kind_;
    uint32_t index_;
    
public:
    ArrayIterator(Object* array, Kind kind);
    
    static std::unique_ptr<ArrayIterator> create_keys_iterator(Object* array);
    static std::unique_ptr<ArrayIterator> create_values_iterator(Object* array);
    static std::unique_ptr<ArrayIterator> create_entries_iterator(Object* array);
    
private:
    IteratorResult next_impl();
};

/**
 * String Iterator implementation
 * Iterates over string characters (Unicode-aware)
 */
class StringIterator : public Iterator {
private:
    std::string string_;
    size_t position_;
    
public:
    StringIterator(const std::string& str);
    
    IteratorResult next() override;
    
    static Value string_iterator_next_method(Context& ctx, const std::vector<Value>& args);
    
private:
    IteratorResult next_impl();
};

/**
 * Map Iterator implementation
 * Iterates over Map entries
 */
class MapIterator : public Iterator {
public:
    enum class Kind {
        Keys,
        Values,
        Entries
    };
    
private:
    class Map* map_;
    Kind kind_;
    size_t index_;
    
public:
    MapIterator(class Map* map, Kind kind);
    
    IteratorResult next() override;
    
    static Value map_iterator_next_method(Context& ctx, const std::vector<Value>& args);
    
private:
    IteratorResult next_impl();
};

/**
 * Set Iterator implementation
 * Iterates over Set values
 */
class SetIterator : public Iterator {
public:
    enum class Kind {
        Values,
        Entries
    };
    
private:
    class Set* set_;
    Kind kind_;
    size_t index_;
    
public:
    SetIterator(class Set* set, Kind kind);
    
    IteratorResult next() override;
    
    static Value set_iterator_next_method(Context& ctx, const std::vector<Value>& args);
    
private:
    IteratorResult next_impl();
};

/**
 * Iterable utilities
 * Helper functions for working with iterables
 */
namespace IterableUtils {
    bool is_iterable(const Value& value);
    
    std::unique_ptr<Iterator> get_iterator(const Value& value, Context& ctx);
    
    std::vector<Value> to_array(const Value& iterable, Context& ctx);
    
    void for_of_loop(const Value& iterable, 
                     std::function<void(const Value&)> callback, 
                     Context& ctx);
    
    void setup_array_iterator_methods(Context& ctx);
    void setup_string_iterator_methods(Context& ctx);
    void setup_map_iterator_methods(Context& ctx);
    void setup_set_iterator_methods(Context& ctx);
    
    std::unique_ptr<Iterator> create_range_iterator(double start, double end, double step = 1.0);
    std::unique_ptr<Iterator> create_filter_iterator(std::unique_ptr<Iterator> source, 
                                                    std::function<bool(const Value&)> predicate);
    std::unique_ptr<Iterator> create_map_iterator(std::unique_ptr<Iterator> source, 
                                                 std::function<Value(const Value&)> mapper);
    std::unique_ptr<Iterator> create_take_iterator(std::unique_ptr<Iterator> source, size_t count);
    std::unique_ptr<Iterator> create_drop_iterator(std::unique_ptr<Iterator> source, size_t count);
}

}
