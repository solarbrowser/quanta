#include "String.h"
#include <unordered_map>

namespace Quanta {

// String interning cache
static std::unordered_map<std::string, std::weak_ptr<std::string>> intern_cache_;

String::String() : data_(std::make_shared<std::string>()), hash_(0), interned_(false) {
}

String::String(const std::string& str) 
    : data_(std::make_shared<std::string>(str)), interned_(false) {
    calculate_hash();
}

String::String(const char* str) 
    : data_(std::make_shared<std::string>(str)), interned_(false) {
    calculate_hash();
}

bool String::operator==(const String& other) const {
    if (data_ == other.data_) return true;
    if (hash_ != other.hash_) return false;
    return str() == other.str();
}

String String::concat(const String& other) const {
    return String(str() + other.str());
}

String String::substring(size_t start, size_t length) const {
    return String(str().substr(start, length));
}

String String::intern(const std::string& str) {
    auto it = intern_cache_.find(str);
    if (it != intern_cache_.end()) {
        auto shared = it->second.lock();
        if (shared) {
            String result;
            result.data_ = shared;
            result.interned_ = true;
            result.calculate_hash();
            return result;
        }
    }
    
    // Create new interned string
    String result(str);
    result.interned_ = true;
    intern_cache_[str] = result.data_;
    return result;
}

void String::calculate_hash() {
    hash_ = std::hash<std::string>{}(*data_);
}

} // namespace Quanta