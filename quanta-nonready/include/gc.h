//<---------QUANTA JS ENGINE - GARBAGE COLLECTION--------->
// Stage 5: Final Optimizations & Library Support - Garbage Collection System
// Purpose: Memory management, object lifecycle tracking, and automatic cleanup
// Max Lines: 3000 (Current: ~200)

#ifndef QUANTA_GC_H
#define QUANTA_GC_H

#include "runtime_objects.h"
#include "hash_workaround.h"
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

namespace Quanta {

//<---------FORWARD DECLARATIONS--------->
class GarbageCollector;
class GCObject;
class GCStats;

//<---------GC OBJECT BASE CLASS--------->
class GCObject {
public:
    GCObject();
    virtual ~GCObject() = default;
    
    // GC metadata
    bool isMarked() const { return marked_; }
    void mark() { marked_ = true; }
    void unmark() { marked_ = false; }
    
    size_t getSize() const { return size_; }
    void setSize(size_t size) { size_ = size; }
    
    // Object references for GC traversal
    virtual std::vector<GCObject*> getReferences() const { return {}; }
    
    // Object type information
    virtual std::string getGCType() const = 0;
    
    // Generation information (for generational GC)
    int getGeneration() const { return generation_; }
    void setGeneration(int gen) { generation_ = gen; }
    
    // Finalization
    virtual void finalize() {}

protected:
    bool marked_ = false;
    size_t size_ = 0;
    int generation_ = 0;
    std::chrono::steady_clock::time_point creationTime_;
    
    friend class GarbageCollector;
};

//<---------MANAGED POINTER--------->
template<typename T>
class GCPtr {
public:
    GCPtr() : ptr_(nullptr) {}
    GCPtr(T* ptr);
    GCPtr(const GCPtr& other);
    GCPtr& operator=(const GCPtr& other);
    ~GCPtr();
    
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }
    
    bool operator==(const GCPtr& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const GCPtr& other) const { return ptr_ != other.ptr_; }
    
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_;
};

//<---------GC STATISTICS--------->
class GCStats {
public:
    size_t totalObjects = 0;
    size_t totalMemory = 0;
    size_t collectionCount = 0;
    size_t objectsCollected = 0;
    size_t memoryFreed = 0;
    std::chrono::milliseconds totalCollectionTime{0};
    std::chrono::milliseconds averageCollectionTime{0};
    
    // Generation statistics
    std::vector<size_t> generationCounts;
    std::vector<size_t> generationSizes;
    
    void reset();
    std::string toString() const;
    void updateAverages();
};

//<---------GARBAGE COLLECTOR--------->
class GarbageCollector {
public:
    enum class CollectionType {
        MINOR,  // Young generation only
        MAJOR,  // All generations
        FULL    // Full collection with compaction
    };
    
    static GarbageCollector& getInstance();
    
    // Object lifecycle
    void registerObject(GCObject* obj);
    void unregisterObject(GCObject* obj);
    
    // Pointer tracking
    void registerPointer(GCObject* obj);
    void unregisterPointer(GCObject* obj);
    
    // Root management
    void addRoot(GCObject* root);
    void removeRoot(GCObject* root);
    
    // Collection control
    void collect(CollectionType type = CollectionType::MINOR);
    void forceCollection();
    void enableAutoCollection(bool enable) { autoCollectionEnabled_ = enable; }
    
    // Memory management
    void* allocate(size_t size);
    void deallocate(void* ptr);
    
    // Configuration
    void setCollectionThreshold(size_t threshold) { collectionThreshold_ = threshold; }
    void setMaxHeapSize(size_t maxSize) { maxHeapSize_ = maxSize; }
    void setGenerationThreshold(int generation, size_t threshold);
    
    // Statistics
    const GCStats& getStats() const { return stats_; }
    void resetStats() { stats_.reset(); }
    
    // Thread safety
    void pauseCollection();
    void resumeCollection();
    
    // Debugging
    void dumpHeap() const;
    void validateHeap() const;
    std::vector<GCObject*> getObjectsByType(const std::string& type) const;

private:
    GarbageCollector();
    ~GarbageCollector();
    
    // Core GC algorithms
    void markAndSweep();
    void markReachableObjects();
    void sweepUnreachableObjects();
    void compactHeap();
    
    // Generational GC
    void promoteObjects();
    void collectGeneration(int generation);
    
    // Mark phase helpers
    void markObject(GCObject* obj);
    void markFromRoots();
    void markFromPointers();
    
    // Sweep phase helpers
    void sweepObjects();
    void finalizeObjects();
    
    // Memory management
    void checkCollectionTrigger();
    bool shouldCollect() const;
    
    // Thread safety
    mutable std::mutex gcMutex_;
    std::atomic<bool> collectionInProgress_{false};
    std::atomic<bool> collectionPaused_{false};
      // Object tracking
    SimpleSet<GCObject*> allObjects_;
    SimpleSet<GCObject*> rootObjects_;
    SimpleMap<GCObject*, size_t> pointerCounts_;
    
    // Generational data
    std::vector<SimpleSet<GCObject*>> generations_;
    std::vector<size_t> generationThresholds_;
    
    // Configuration
    size_t collectionThreshold_ = 1024 * 1024; // 1MB
    size_t maxHeapSize_ = 64 * 1024 * 1024;    // 64MB
    bool autoCollectionEnabled_ = true;
    
    // Statistics
    GCStats stats_;
    std::chrono::steady_clock::time_point lastCollection_;
    
    // Background collection thread
    std::thread collectionThread_;
    std::atomic<bool> shutdown_{false};
    void backgroundCollectionLoop();
};

//<---------RAII GC GUARD--------->
class GCGuard {
public:
    GCGuard() {
        GarbageCollector::getInstance().pauseCollection();
    }
    
    ~GCGuard() {
        GarbageCollector::getInstance().resumeCollection();
    }
    
    GCGuard(const GCGuard&) = delete;
    GCGuard& operator=(const GCGuard&) = delete;
};

//<---------GC UTILITY FUNCTIONS--------->
template<typename T, typename... Args>
GCPtr<T> makeGC(Args&&... args) {
    static_assert(std::is_base_of_v<GCObject, T>, "T must inherit from GCObject");
    return GCPtr<T>(new T(std::forward<Args>(args)...));
}

void enableGC();
void disableGC();
void collectGarbage();
GCStats getGCStats();

//<---------GC INTEGRATION WITH RUNTIME OBJECTS--------->
class GCManagedJSObject : public JSObject, public GCObject {
public:
    GCManagedJSObject() = default;
    
    std::vector<GCObject*> getReferences() const override;
    std::string getGCType() const override { return "JSObject"; }
    
    // Override JSObject methods to update GC metadata
    void setProperty(const std::string& name, const JSValue& value) override;
    JSValue getProperty(const std::string& name) override;

private:    void updateGCReferences();
};

//<---------GC POINTER TEMPLATE IMPLEMENTATIONS--------->
template<typename T>
GCPtr<T>::GCPtr(T* ptr) : ptr_(ptr) {
    if (ptr_) {
        GarbageCollector::getInstance().registerPointer(ptr_);
    }
}

template<typename T>
GCPtr<T>::GCPtr(const GCPtr& other) : ptr_(other.ptr_) {
    if (ptr_) {
        GarbageCollector::getInstance().registerPointer(ptr_);
    }
}

template<typename T>
GCPtr<T>& GCPtr<T>::operator=(const GCPtr& other) {
    if (this != &other) {
        if (ptr_) {
            GarbageCollector::getInstance().unregisterPointer(ptr_);
        }
        ptr_ = other.ptr_;
        if (ptr_) {
            GarbageCollector::getInstance().registerPointer(ptr_);
        }
    }
    return *this;
}

template<typename T>
GCPtr<T>::~GCPtr() {
    if (ptr_) {
        GarbageCollector::getInstance().unregisterPointer(ptr_);
    }
}

} // namespace Quanta

#endif // QUANTA_GC_H