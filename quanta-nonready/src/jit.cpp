// Stage 3: JIT Compiler & Optimizer - Main JIT Implementation
// Purpose: Just-In-Time compilation for hot code paths
// Max Lines: 5000 (Current: ~800)

#include "../include/jit.h"
#include "../include/ir.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace Quanta {

//<---------HOT PATH PROFILER IMPLEMENTATION--------->
HotPathProfiler::HotPathProfiler(size_t threshold, double minTime) 
    : hotThreshold(threshold), minExecutionTime(minTime) {}

void HotPathProfiler::recordCall(const ASTNode* node, double executionTime) {
    if (!node) return;
    
    ProfileData& profile = profiles[node];
    profile.callCount++;
    profile.totalTime += executionTime;
    profile.averageTime = profile.totalTime / profile.callCount;
    
    // Mark as hot if it meets the criteria
    if (profile.callCount >= hotThreshold && profile.averageTime >= minExecutionTime) {
        profile.isHot = true;
    }
}

bool HotPathProfiler::isHotPath(const ASTNode* node) const {
    auto it = profiles.find(node);
    return it != profiles.end() && it->second.isHot;
}

HotPathProfiler::ProfileData HotPathProfiler::getProfile(const ASTNode* node) const {
    auto it = profiles.find(node);
    return it != profiles.end() ? it->second : ProfileData{};
}

void HotPathProfiler::clearProfiles() {
    profiles.clear();
}

std::vector<const ASTNode*> HotPathProfiler::getHotPaths() const {
    std::vector<const ASTNode*> hotPaths;
    for (const auto& pair : profiles) {
        if (pair.second.isHot) {
            hotPaths.push_back(pair.first);
        }
    }
    return hotPaths;
}

//<---------JIT COMPILER IMPLEMENTATION--------->
JITCompiler::JITCompiler(ErrorHandler& errorHandler) : errorHandler(errorHandler) {
    irGenerator = std::make_shared<IRGenerator>(errorHandler);
    profiler = std::make_shared<HotPathProfiler>(hotThreshold, minExecutionTime);
}

JSValue JITCompiler::executeFunction(const ASTNode* functionNode, 
                                   const std::vector<JSValue>& args,
                                   ScopeManager& scopeManager) {
    if (!functionNode) {
        return nullptr;
    }
    
    // Suppress unused parameter warnings for now
    (void)args;
    (void)scopeManager;
    
    // Measure execution time for profiling
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Check if we have a compiled version
    auto hotFuncIt = hotFunctions.find(functionNode);
    if (hotFuncIt != hotFunctions.end() && hotFuncIt->second->isCompiled) {
        // Execute compiled version
        auto endTime = std::chrono::high_resolution_clock::now();
        double executionTime = std::chrono::duration<double>(endTime - startTime).count();
        
        hotFuncIt->second->callCount++;
        hotFuncIt->second->totalExecutionTime += executionTime;
        hotFuncIt->second->stats.executionCount++;
        
        try {
            return hotFuncIt->second->compiledCode(args);
        } catch (const std::exception& e) {
            errorHandler.reportRuntimeError("JIT execution failed: " + std::string(e.what()), 0, 0);
            return nullptr;
        }
    }
    
    // Execute in interpreter mode and record profiling data
    JSValue result = nullptr; // Would call interpreter here
    
    auto endTime = std::chrono::high_resolution_clock::now();
    double executionTime = std::chrono::duration<double>(endTime - startTime).count();
    
    recordExecution(functionNode, executionTime);
    
    // Check if we should compile this function
    if (shouldCompile(functionNode)) {
        compileFunction(functionNode);
    }
    
    return result;
}

void JITCompiler::recordExecution(const ASTNode* node, double executionTime) {
    profiler->recordCall(node, executionTime);
    
    // Update or create hot function entry
    auto it = hotFunctions.find(node);
    if (it == hotFunctions.end()) {
        hotFunctions[node] = std::make_shared<HotFunction>(node);
        it = hotFunctions.find(node);
    }
    
    it->second->callCount++;
    it->second->totalExecutionTime += executionTime;
    
    // Update global stats
    globalStats.executionCount++;
}

bool JITCompiler::shouldCompile(const ASTNode* node) {
    if (!enableOptimizations) return false;
    
    auto it = hotFunctions.find(node);
    if (it == hotFunctions.end()) return false;
    
    const auto& hotFunc = it->second;
    return !hotFunc->isCompiled && 
           hotFunc->callCount >= hotThreshold &&
           hotFunc->totalExecutionTime >= minExecutionTime;
}

void JITCompiler::compileFunction(const ASTNode* node) {
    if (!node) return;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        // Generate IR
        std::string functionName = "jit_func_" + std::to_string(reinterpret_cast<uintptr_t>(node));
        auto irFunction = irGenerator->generateIR(node, functionName);
        
        if (!irFunction) {
            errorHandler.reportRuntimeError("Failed to generate IR for JIT compilation", 0, 0);
            return;
        }
          // Optimize IR
        IROptimizer::optimizeFunction(*irFunction);
        
        // For now, create a simple interpreter for the IR
        // In a full implementation, this would emit native code
        auto sharedIR = std::shared_ptr<IRFunction>(irFunction.release());
        CompiledFunction compiledCode = [sharedIR](const std::vector<JSValue>& args) -> JSValue {
            // Simple IR interpreter execution
            // This is a placeholder - real JIT would emit native code
            (void)args; // Suppress unused parameter warning
            return 42.0; // Dummy result
        };
        
        // Update hot function
        auto it = hotFunctions.find(node);
        if (it != hotFunctions.end()) {
            it->second->isCompiled = true;
            it->second->compiledCode = compiledCode;
            
            auto endTime = std::chrono::high_resolution_clock::now();
            double compilationTime = std::chrono::duration<double>(endTime - startTime).count();
            
            it->second->stats.compilationTime = compilationTime;
            it->second->stats.compilationsCount++;
            
            globalStats.compilationsCount++;
        }
        
        std::cout << "JIT: Compiled function (IR instructions: " 
                  << irFunction->getTotalInstructions() << ")" << std::endl;
        
    } catch (const std::exception& e) {
        errorHandler.reportRuntimeError("JIT compilation failed: " + std::string(e.what()), 0, 0);
    }
}

JITStats JITCompiler::getFunctionStats(const ASTNode* node) const {
    auto it = hotFunctions.find(node);
    return it != hotFunctions.end() ? it->second->stats : JITStats{};
}

void JITCompiler::printHotFunctions() const {
    std::cout << "\n//<---------JIT HOT FUNCTIONS--------->\\n";
    
    std::vector<std::pair<const ASTNode*, std::shared_ptr<HotFunction>>> sortedFunctions;
    for (const auto& pair : hotFunctions) {
        sortedFunctions.push_back(pair);
    }
    
    // Sort by call count
    std::sort(sortedFunctions.begin(), sortedFunctions.end(),
        [](const auto& a, const auto& b) {
            return a.second->callCount > b.second->callCount;
        });
    
    for (const auto& pair : sortedFunctions) {
        const auto& hotFunc = pair.second;
        std::cout << "Function at " << pair.first 
                  << " - Calls: " << hotFunc->callCount
                  << ", Avg Time: " << (hotFunc->totalExecutionTime / hotFunc->callCount)
                  << "s, Compiled: " << (hotFunc->isCompiled ? "Yes" : "No") << std::endl;
    }
}

void JITCompiler::clearStats() {
    hotFunctions.clear();
    globalStats = JITStats{};
    profiler->clearProfiles();
}

void JITCompiler::cleanupUnusedFunctions() {
    // Remove functions that haven't been called recently
    // This is a simple implementation - real GC would be more sophisticated
    auto it = hotFunctions.begin();
    while (it != hotFunctions.end()) {
        if (it->second->callCount == 0) {
            it = hotFunctions.erase(it);
        } else {
            // Reset call count for next cleanup cycle
            it->second->callCount = 0;
            ++it;
        }
    }
}

//<---------JIT COMPILER FACTORY IMPLEMENTATION--------->
std::unique_ptr<JITCompiler> JITCompilerFactory::createCompiler(ErrorHandler& errorHandler) {
    return std::make_unique<JITCompiler>(errorHandler);
}

std::unique_ptr<JITCompiler> JITCompilerFactory::createOptimizedCompiler(ErrorHandler& errorHandler) {
    auto compiler = std::make_unique<JITCompiler>(errorHandler);
    compiler->enableOptimization(true);
    compiler->setHotThreshold(5); // Lower threshold for optimization build
    return compiler;
}

std::unique_ptr<JITCompiler> JITCompilerFactory::createDebugCompiler(ErrorHandler& errorHandler) {
    auto compiler = std::make_unique<JITCompiler>(errorHandler);
    compiler->enableOptimization(false);
    compiler->setHotThreshold(100); // Higher threshold for debug build
    return compiler;
}

//<---------UTILITY FUNCTIONS IMPLEMENTATION--------->
std::string jitStatsToString(const JITStats& stats) {
    std::stringstream ss;
    ss << "JIT Statistics:\n";
    ss << "  Compilations: " << stats.compilationsCount << "\n";
    ss << "  Executions: " << stats.executionCount << "\n";
    ss << "  Bytecode Size: " << stats.bytecodeSize << " bytes\n";
    ss << "  Native Code Size: " << stats.nativeCodeSize << " bytes\n";
    ss << "  Average Execution Time: " << stats.averageExecutionTime << " ms\n";
    ss << "  Compilation Time: " << stats.compilationTime << " ms";
    return ss.str();
}

double measureExecutionTime(const std::function<void()>& func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

bool isJITSupported() {
    // For this implementation, JIT is always supported
    // In a real implementation, this would check CPU features, OS support, etc.
    return true;
}

} // namespace Quanta
