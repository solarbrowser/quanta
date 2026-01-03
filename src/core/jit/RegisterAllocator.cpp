#include "quanta/core/jit/RegisterAllocator.h"
#include <iostream>
#include <algorithm>

namespace Quanta {

RegisterAllocator::RegisterAllocator() : next_spill_slot_(0) {
    available_regs_ = {
        X64Register::RAX, X64Register::RCX, X64Register::RDX,
        X64Register::R8, X64Register::R9, X64Register::R10,
        X64Register::R11, X64Register::R12, X64Register::R13,
        X64Register::R14, X64Register::R15
    };
}

void RegisterAllocator::compute_live_intervals(void* function) {
    std::cout << "[REG-ALLOC] Computing live intervals" << std::endl;
}

X64Register RegisterAllocator::allocate(const std::string& var_name) {
    auto it = allocation_.find(var_name);
    if (it != allocation_.end()) {
        return it->second;
    }

    X64Register reg = find_free_register();
    allocation_[var_name] = reg;

    std::cout << "[REG-ALLOC] Allocated register for variable: " << var_name << std::endl;

    return reg;
}

void RegisterAllocator::free_register(X64Register reg) {
    auto it = std::find(available_regs_.begin(), available_regs_.end(), reg);
    if (it == available_regs_.end()) {
        available_regs_.push_back(reg);
    }
}

bool RegisterAllocator::needs_spill() {
    return available_regs_.empty();
}

X64Register RegisterAllocator::get_allocation(const std::string& var_name) {
    auto it = allocation_.find(var_name);
    return (it != allocation_.end()) ? it->second : X64Register::RAX;
}

void RegisterAllocator::linear_scan() {
    std::sort(intervals_.begin(), intervals_.end(),
        [](const LiveInterval& a, const LiveInterval& b) {
            return a.start < b.start;
        });

    std::cout << "[REG-ALLOC] Running linear scan register allocation" << std::endl;

    for (auto& interval : intervals_) {
        if (available_regs_.empty()) {
            spill_register(interval);
        } else {
            interval.assigned_reg = available_regs_.back();
            available_regs_.pop_back();
            interval.spilled = false;
        }
    }
}

void RegisterAllocator::spill_register(LiveInterval& interval) {
    interval.spilled = true;
    std::cout << "[REG-ALLOC] Spilling variable: " << interval.var_name
              << " to stack slot " << next_spill_slot_ << std::endl;
    next_spill_slot_++;
}

X64Register RegisterAllocator::find_free_register() {
    if (available_regs_.empty()) {
        std::cout << "[REG-ALLOC] No free registers - must spill" << std::endl;
        return X64Register::RAX;
    }

    X64Register reg = available_regs_.back();
    available_regs_.pop_back();
    return reg;
}

}
