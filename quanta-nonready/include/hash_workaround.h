//<---------QUANTA JS ENGINE - HASH WORKAROUND--------->
// Stage 5: Workaround for GCC 15.1.0 std::hash issues
// Purpose: Provide working hash map alternatives

#ifndef QUANTA_HASH_WORKAROUND_H
#define QUANTA_HASH_WORKAROUND_H

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace Quanta {

//<---------CUSTOM STRING HASH FUNCTION--------->
struct StringHash {
    std::size_t operator()(const std::string& str) const noexcept {
        std::size_t hash = 5381;
        for (char c : str) {
            hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
        }
        return hash;
    }
};

//<---------SIMPLE MAP IMPLEMENTATION--------->
template<typename Key, typename Value>
class SimpleMap {
private:
    struct Entry {
        Key key;
        Value value;
        bool used = false;
        
        Entry() = default;
        Entry(const Key& k, const Value& v) : key(k), value(v), used(true) {}
    };
    
    std::vector<Entry> buckets;
    size_t bucket_count;
    size_t size_;
    
    size_t hash(const Key& key) const {
        if constexpr (std::is_same_v<Key, std::string>) {
            return StringHash{}(key) % bucket_count;
        } else {
            return std::hash<Key>{}(key) % bucket_count;
        }
    }
    
    void rehash() {
        if (size_ * 2 > bucket_count) {
            std::vector<Entry> old_buckets = std::move(buckets);
            bucket_count *= 2;
            buckets = std::vector<Entry>(bucket_count);
            size_ = 0;
            
            for (const auto& entry : old_buckets) {
                if (entry.used) {
                    insert(entry.key, entry.value);
                }
            }
        }
    }

public:
    SimpleMap() : bucket_count(16), size_(0) {
        buckets.resize(bucket_count);
    }
    
    void insert(const Key& key, const Value& value) {
        rehash();
        
        size_t index = hash(key);
        size_t original_index = index;
        
        do {
            if (!buckets[index].used) {
                buckets[index] = Entry(key, value);
                size_++;
                return;
            }
            if (buckets[index].key == key) {
                buckets[index].value = value;
                return;
            }
            index = (index + 1) % bucket_count;
        } while (index != original_index);
    }
    
    Value* find(const Key& key) {
        size_t index = hash(key);
        size_t original_index = index;
        
        do {
            if (!buckets[index].used) {
                return nullptr;
            }
            if (buckets[index].key == key) {
                return &buckets[index].value;
            }
            index = (index + 1) % bucket_count;
        } while (index != original_index);
        
        return nullptr;
    }
    
    const Value* find(const Key& key) const {
        size_t index = hash(key);
        size_t original_index = index;
        
        do {
            if (!buckets[index].used) {
                return nullptr;
            }
            if (buckets[index].key == key) {
                return &buckets[index].value;
            }
            index = (index + 1) % bucket_count;
        } while (index != original_index);
        
        return nullptr;
    }
    
    Value& operator[](const Key& key) {
        Value* found = find(key);
        if (found) {
            return *found;
        }
        
        insert(key, Value{});
        return *find(key);
    }
    
    const Value& at(const Key& key) const {
        const Value* found = find(key);
        if (!found) {
            throw std::out_of_range("Key not found");
        }
        return *found;
    }
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    void clear() {
        buckets.clear();
        buckets.resize(bucket_count);
        size_ = 0;
    }
    
    // Iterator support for range-based for loops
    class iterator {
    private:
        std::vector<Entry>* buckets_;
        size_t index_;
        
        void advance_to_used() {
            while (index_ < buckets_->size() && !(*buckets_)[index_].used) {
                ++index_;
            }
        }
        
    public:
        iterator(std::vector<Entry>* buckets, size_t index) 
            : buckets_(buckets), index_(index) {
            advance_to_used();
        }
        
        iterator& operator++() {
            ++index_;
            advance_to_used();
            return *this;
        }
        
        bool operator!=(const iterator& other) const {
            return index_ != other.index_;
        }
        
        std::pair<const Key&, Value&> operator*() {
            return {(*buckets_)[index_].key, (*buckets_)[index_].value};
        }
    };
    
    iterator begin() {
        return iterator(&buckets, 0);
    }
    
    iterator end() {
        return iterator(&buckets, buckets.size());
    }
};

} // namespace Quanta

//<---------SIMPLE SET IMPLEMENTATION--------->
namespace Quanta {

template<typename Key>
class SimpleSet {
private:
    std::vector<Key> items;
    
public:
    void insert(const Key& key) {
        if (find(key) == items.end()) {
            items.push_back(key);
        }
    }
    
    void erase(const Key& key) {
        auto it = find(key);
        if (it != items.end()) {
            items.erase(it);
        }
    }
    
    typename std::vector<Key>::iterator find(const Key& key) {
        return std::find(items.begin(), items.end(), key);
    }
    
    typename std::vector<Key>::const_iterator find(const Key& key) const {
        return std::find(items.begin(), items.end(), key);
    }
    
    bool count(const Key& key) const {
        return find(key) != items.end();
    }
    
    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }
    
    typename std::vector<Key>::iterator begin() { return items.begin(); }
    typename std::vector<Key>::iterator end() { return items.end(); }
    typename std::vector<Key>::const_iterator begin() const { return items.begin(); }
    typename std::vector<Key>::const_iterator end() const { return items.end(); }
};

} // namespace Quanta

#endif // QUANTA_HASH_WORKAROUND_H
