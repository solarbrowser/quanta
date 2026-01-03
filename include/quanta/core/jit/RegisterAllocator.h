#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace Quanta {

enum class X64Register {
    RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP,
    R8, R9, R10, R11, R12, R13, R14, R15,
    XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7
};

struct LiveInterval {
    std::string var_name;
    int start;
    int end;
    X64Register assigned_reg;
    bool spilled;
};

class RegisterAllocator {
private:
    std::vector<LiveInterval> intervals_;
    std::unordered_map<std::string, X64Register> allocation_;
    std::vector<X64Register> available_regs_;
    int next_spill_slot_;

public:
    RegisterAllocator();

    void compute_live_intervals(void* function);
    X64Register allocate(const std::string& var_name);
    void free_register(X64Register reg);
    bool needs_spill();

    X64Register get_allocation(const std::string& var_name);

private:
    void linear_scan();
    void spill_register(LiveInterval& interval);
    X64Register find_free_register();
};

}
