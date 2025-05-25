//<---------QUANTA JS ENGINE - GARBAGE COLLECTION IMPLEMENTATION--------->
// Stage 5: Final Optimizations & Library Support - Garbage Collection System
// Purpose: Memory management, object lifecycle tracking, and automatic cleanup

#include "../include/gc.h"
#include <iostream>
#include <algorithm>
#include <sstream>

namespace Quanta {

//<---------GC OBJECT IMPLEMENTATION--------->
GCObject::GCObject() : creationTime_(std::chrono::steady_clock::now()) {
    GarbageCollector::getInstance().registerObject(this);
}

//<---------GC STATS IMPLEMENTATION--------->
void GCStats::reset() {
    totalObjects = 0;
    totalMemory = 0;
    collectionCount = 0;
    objectsCollected = 0;
    memoryFreed = 0;
    totalCollectionTime = std::chrono::milliseconds{0};
    averageCollectionTime = std::chrono::milliseconds{0};
    generationCounts.clear();
    generationSizes.clear();
}

std::string GCStats::toString() const {
    std::stringstream ss;
    ss << "GC Statistics:\n";
    ss << "  Total Objects: " << totalObjects << "\n";
    ss << "  Total Memory: " << totalMemory << " bytes\n";
    ss << "  Collections: " << collectionCount << "\n";
    ss << "  Objects Collected: " << objectsCollected << "\n";
    ss << "  Memory Freed: " << memoryFreed << " bytes\n";
    ss << "  Total Collection Time: " << totalCollectionTime.count() << " ms\n";
    ss << "  Average Collection Time: " << averageCollectionTime.count() << " ms\n";
    
    if (!generationCounts.empty()) {
        ss << "  Generation Statistics:\n";
        for (size_t i = 0; i < generationCounts.size(); ++i) {
            ss << "    Gen " << i << ": " << generationCounts[i] 
               << " objects, " << generationSizes[i] << " bytes\n";
        }
    }
    
    return ss.str();
}

void GCStats::updateAverages() {
    if (collectionCount > 0) {
        averageCollectionTime = std::chrono::milliseconds(
            totalCollectionTime.count() / collectionCount
        );
    }
}

//<---------GARBAGE COLLECTOR IMPLEMENTATION--------->
GarbageCollector& GarbageCollector::getInstance() {
    static GarbageCollector instance;
    return instance;
}

GarbageCollector::GarbageCollector() {
    // Initialize generations (3 generations: young, old, permanent)
    generations_.resize(3);
    generationThresholds_ = {64 * 1024, 512 * 1024, 2 * 1024 * 1024}; // 64KB, 512KB, 2MB
    
    stats_.generationCounts.resize(3, 0);
    stats_.generationSizes.resize(3, 0);
    
    lastCollection_ = std::chrono::steady_clock::now();
    
    // Start background collection thread
    collectionThread_ = std::thread(&GarbageCollector::backgroundCollectionLoop, this);
}

GarbageCollector::~GarbageCollector() {
    shutdown_ = true;
    if (collectionThread_.joinable()) {
        collectionThread_.join();
    }
}

void GarbageCollector::registerObject(GCObject* obj) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (obj) {
        allObjects_.insert(obj);
        generations_[0].insert(obj); // Start in young generation
        obj->setGeneration(0);
        
        stats_.totalObjects++;
        stats_.generationCounts[0]++;
        stats_.totalMemory += obj->getSize();
        stats_.generationSizes[0] += obj->getSize();
        
        checkCollectionTrigger();
    }
}

void GarbageCollector::unregisterObject(GCObject* obj) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (obj) {
        allObjects_.erase(obj);
        int gen = obj->getGeneration();
        if (gen >= 0 && gen < static_cast<int>(generations_.size())) {
            generations_[gen].erase(obj);
            stats_.generationCounts[gen]--;
            stats_.generationSizes[gen] -= obj->getSize();
        }
        stats_.totalObjects--;
        stats_.totalMemory -= obj->getSize();
    }
}

void GarbageCollector::registerPointer(GCObject* obj) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (obj) {
        pointerCounts_[obj]++;
    }
}

void GarbageCollector::unregisterPointer(GCObject* obj) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (obj) {
        auto it = pointerCounts_.find(obj);
        if (it != pointerCounts_.end()) {
            it->second--;
            if (it->second == 0) {
                pointerCounts_.erase(it);
            }
        }
    }
}

void GarbageCollector::addRoot(GCObject* root) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    if (root) {
        rootObjects_.insert(root);
    }
}

void GarbageCollector::removeRoot(GCObject* root) {
    std::lock_guard<std::mutex> lock(gcMutex_);
    rootObjects_.erase(root);
}

void GarbageCollector::collect(CollectionType type) {
    if (collectionInProgress_ || collectionPaused_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(gcMutex_);
    collectionInProgress_ = true;
    
    auto startTime = std::chrono::steady_clock::now();
    
    switch (type) {
        case CollectionType::MINOR:
            collectGeneration(0); // Only young generation
            break;
        case CollectionType::MAJOR:
            markAndSweep(); // All generations
            break;
        case CollectionType::FULL:
            markAndSweep();
            compactHeap();
            break;
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    stats_.collectionCount++;
    stats_.totalCollectionTime += duration;
    stats_.updateAverages();
    lastCollection_ = endTime;
    
    collectionInProgress_ = false;
}

void GarbageCollector::forceCollection() {
    collect(CollectionType::FULL);
}

void GarbageCollector::markAndSweep() {
    // Mark phase
    markReachableObjects();
    
    // Sweep phase
    sweepUnreachableObjects();
    
    // Promote surviving objects to older generations
    promoteObjects();
}

void GarbageCollector::markReachableObjects() {
    // Clear all marks
    for (GCObject* obj : allObjects_) {
        obj->unmark();
    }
    
    // Mark from roots
    markFromRoots();
    
    // Mark from live pointers
    markFromPointers();
}

void GarbageCollector::markFromRoots() {
    for (GCObject* root : rootObjects_) {
        markObject(root);
    }
}

void GarbageCollector::markFromPointers() {
    for (const auto& pair : pointerCounts_) {
        if (pair.second > 0) {
            markObject(pair.first);
        }
    }
}

void GarbageCollector::markObject(GCObject* obj) {
    if (!obj || obj->isMarked()) {
        return;
    }
    
    obj->mark();
    
    // Mark referenced objects
    std::vector<GCObject*> references = obj->getReferences();
    for (GCObject* ref : references) {
        markObject(ref);
    }
}

void GarbageCollector::sweepUnreachableObjects() {
    std::vector<GCObject*> toDelete;
    
    for (GCObject* obj : allObjects_) {
        if (!obj->isMarked()) {
            toDelete.push_back(obj);
        }
    }
    
    for (GCObject* obj : toDelete) {
        // Finalize object
        obj->finalize();
        
        // Update statistics
        stats_.objectsCollected++;
        stats_.memoryFreed += obj->getSize();
        
        // Remove from tracking
        unregisterObject(obj);
        
        // Delete object
        delete obj;
    }
}

void GarbageCollector::promoteObjects() {
    // Move surviving objects to older generations
    for (int gen = 0; gen < static_cast<int>(generations_.size()) - 1; ++gen) {
        std::vector<GCObject*> toPromote;
        
        for (GCObject* obj : generations_[gen]) {
            if (obj->isMarked()) {
                // Simple promotion criteria: survived multiple collections
                auto age = std::chrono::steady_clock::now() - obj->creationTime_;
                if (age > std::chrono::seconds(1)) { // Survived for 1 second
                    toPromote.push_back(obj);
                }
            }
        }
        
        for (GCObject* obj : toPromote) {
            generations_[gen].erase(obj);
            generations_[gen + 1].insert(obj);
            obj->setGeneration(gen + 1);
            
            stats_.generationCounts[gen]--;
            stats_.generationCounts[gen + 1]++;
            stats_.generationSizes[gen] -= obj->getSize();
            stats_.generationSizes[gen + 1] += obj->getSize();
        }
    }
}

void GarbageCollector::collectGeneration(int generation) {
    if (generation < 0 || generation >= static_cast<int>(generations_.size())) {
        return;
    }
    
    // Mark objects in this generation and younger
    for (GCObject* obj : allObjects_) {
        if (obj->getGeneration() <= generation) {
            obj->unmark();
        }
    }
    
    // Mark from roots and pointers
    markFromRoots();
    markFromPointers();
    
    // Sweep unreachable objects in this generation
    std::vector<GCObject*> toDelete;
    for (GCObject* obj : generations_[generation]) {
        if (!obj->isMarked()) {
            toDelete.push_back(obj);
        }
    }
    
    for (GCObject* obj : toDelete) {
        obj->finalize();
        stats_.objectsCollected++;
        stats_.memoryFreed += obj->getSize();
        unregisterObject(obj);
        delete obj;
    }
}

void GarbageCollector::compactHeap() {
    // Simple compaction: just update statistics for now
    // In a real implementation, this would move objects to reduce fragmentation
    stats_.updateAverages();
}

void GarbageCollector::checkCollectionTrigger() {
    if (autoCollectionEnabled_ && shouldCollect()) {
        // Schedule collection in background thread
        if (!collectionInProgress_) {
            // Trigger minor collection for young generation
            collect(CollectionType::MINOR);
        }
    }
}

bool GarbageCollector::shouldCollect() const {
    return stats_.totalMemory > collectionThreshold_;
}

void GarbageCollector::pauseCollection() {
    collectionPaused_ = true;
}

void GarbageCollector::resumeCollection() {
    collectionPaused_ = false;
}

void GarbageCollector::backgroundCollectionLoop() {
    while (!shutdown_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (!collectionPaused_ && autoCollectionEnabled_ && shouldCollect()) {
            collect(CollectionType::MINOR);
        }
        
        // Periodic major collection
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastCollection = now - lastCollection_;
        if (timeSinceLastCollection > std::chrono::seconds(30)) {
            collect(CollectionType::MAJOR);
        }
    }
}

void GarbageCollector::dumpHeap() const {
    std::lock_guard<std::mutex> lock(gcMutex_);
    
    std::cout << "=== Heap Dump ===" << std::endl;
    std::cout << stats_.toString() << std::endl;
    
    std::cout << "Objects by generation:" << std::endl;
    for (size_t i = 0; i < generations_.size(); ++i) {
        std::cout << "  Generation " << i << ": " << generations_[i].size() << " objects" << std::endl;
    }
    
    std::cout << "Root objects: " << rootObjects_.size() << std::endl;
    std::cout << "Live pointers: " << pointerCounts_.size() << std::endl;
}

void GarbageCollector::validateHeap() const {
    std::lock_guard<std::mutex> lock(gcMutex_);
    
    // Validate that all objects are properly tracked
    size_t totalGenObjects = 0;
    for (const auto& gen : generations_) {
        totalGenObjects += gen.size();
    }
    
    if (totalGenObjects != allObjects_.size()) {
        std::cerr << "Heap validation failed: generation count mismatch" << std::endl;
    }
    
    // Validate generation assignments
    for (GCObject* obj : allObjects_) {
        int gen = obj->getGeneration();
        if (gen < 0 || gen >= static_cast<int>(generations_.size())) {
            std::cerr << "Heap validation failed: invalid generation " << gen << std::endl;
        } else if (generations_[gen].find(obj) == generations_[gen].end()) {
            std::cerr << "Heap validation failed: object not in correct generation" << std::endl;
        }
    }
}

std::vector<GCObject*> GarbageCollector::getObjectsByType(const std::string& type) const {
    std::lock_guard<std::mutex> lock(gcMutex_);
    
    std::vector<GCObject*> result;
    for (GCObject* obj : allObjects_) {
        if (obj->getGCType() == type) {
            result.push_back(obj);
        }
    }
    return result;
}

//<---------GC MANAGED JS OBJECT IMPLEMENTATION--------->
std::vector<GCObject*> GCManagedJSObject::getReferences() const {
    std::vector<GCObject*> refs;
    
    // Add references from properties that are GC objects
    for (const auto& prop : properties_) {
        // This would need to be implemented based on JSValue structure
        // For now, return empty vector
    }
    
    return refs;
}

void GCManagedJSObject::setProperty(const std::string& name, const JSValue& value) {
    JSObject::setProperty(name, value);
    updateGCReferences();
}

JSValue GCManagedJSObject::getProperty(const std::string& name) {
    return JSObject::getProperty(name);
}

void GCManagedJSObject::updateGCReferences() {
    // Update GC tracking when properties change
    // This would notify the GC about new references
}

//<---------UTILITY FUNCTIONS--------->
void enableGC() {
    GarbageCollector::getInstance().enableAutoCollection(true);
}

void disableGC() {
    GarbageCollector::getInstance().enableAutoCollection(false);
}

void collectGarbage() {
    GarbageCollector::getInstance().forceCollection();
}

GCStats getGCStats() {
    return GarbageCollector::getInstance().getStats();
}

} // namespace Quanta