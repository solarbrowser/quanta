#pragma once

#include "Value.h"
#include "Object.h"
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
    virtual ~Iterator() = default;
    
    // Iterator protocol methods
    IteratorResult next();
    
    // Iterator built-in methods
    static Value iterator_next(Context& ctx, const std::vector<Value>& args);
    static Value iterator_return(Context& ctx, const std::vector<Value>& args);
    static Value iterator_throw(Context& ctx, const std::vector<Value>& args);
    
    // Setup iterator prototype
    static void setup_iterator_prototype(Context& ctx);
    
    // Helper to create iterator result objects
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
    
    // Create array iterators
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
    
private:
    IteratorResult next_impl();
};

/**
 * Iterable utilities
 * Helper functions for working with iterables
 */
namespace IterableUtils {
    // Check if an object is iterable
    bool is_iterable(const Value& value);
    
    // Get iterator from an iterable
    std::unique_ptr<Iterator> get_iterator(const Value& value, Context& ctx);
    
    // Convert iterable to array
    std::vector<Value> to_array(const Value& iterable, Context& ctx);
    
    // For-of loop implementation
    void for_of_loop(const Value& iterable, 
                     std::function<void(const Value&)> callback, 
                     Context& ctx);
    
    // Built-in iterables setup
    void setup_array_iterator_methods(Context& ctx);
    void setup_string_iterator_methods(Context& ctx);
    void setup_map_iterator_methods(Context& ctx);
    void setup_set_iterator_methods(Context& ctx);
    
    // ES2026 iterator enhancements
    std::unique_ptr<Iterator> create_range_iterator(double start, double end, double step = 1.0);
    std::unique_ptr<Iterator> create_filter_iterator(std::unique_ptr<Iterator> source, 
                                                    std::function<bool(const Value&)> predicate);
    std::unique_ptr<Iterator> create_map_iterator(std::unique_ptr<Iterator> source, 
                                                 std::function<Value(const Value&)> mapper);
    std::unique_ptr<Iterator> create_take_iterator(std::unique_ptr<Iterator> source, size_t count);
    std::unique_ptr<Iterator> create_drop_iterator(std::unique_ptr<Iterator> source, size_t count);
}

} // namespace Quanta