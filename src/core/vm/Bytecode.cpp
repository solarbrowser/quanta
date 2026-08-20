/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/vm/Bytecode.h"
#include "quanta/parser/AST.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/parser/FunctionExecutable.h"
#include <sstream>
#include <cstdio>
#include <cstdlib>

namespace Quanta {

// closures and treewalk_nodes are deliberately two vectors, not one: closures
// carries prebuilt ClosureTemplates, while treewalk_nodes goes away entirely
// once the compiler can emit every construct that currently escapes
// (Op::EvalAst).
#if defined(__GLIBCXX__)
static_assert(sizeof(BytecodeChunk) == 128);
#else
static_assert(sizeof(BytecodeChunk) <= 192);
#endif

BytecodeChunk::BytecodeChunk() = default;
BytecodeChunk::~BytecodeChunk() = default;

std::vector<ClosureTemplate>& BytecodeChunk::ensure_closures() {
    if (!closures) closures = std::make_unique<std::vector<ClosureTemplate>>();
    return *closures;
}

void BytecodeChunk::trace(Visitor& v) const {
    for (const auto& c : constants) {
        v.visit(c);
    }
    // feedback's Shape* fields need no tracing (immortal, not a GC cell), but
    // every Object* in a cache entry is a real cell.
    for (const auto& fb : feedback) {
        for (uint8_t i = 0; i < fb.proto_count; i++) {
            v.visit_object(fb.proto_entries[i].holder);
            v.visit_object(fb.proto_entries[i].prototype);
            // A descriptor-backed entry caches the value itself rather than
            // pointing at a shape slot, so it is a reference of its own.
            v.visit(fb.proto_entries[i].cached_value);
        }
        for (uint8_t i = 0; i < fb.transition_count; i++) {
            v.visit_object(fb.transitions[i].prototype);
        }
        // The primitive-prototype entry holds no Value, but its descriptor
        // lives inside the prototype's own map, so the prototype has to stay
        // alive for the pointer to mean anything.
        v.visit_object(fb.prim_proto);
        v.visit(fb.prim_value);
        // The own-descriptor entry names the receiver it was learned from and
        // caches the value out of that receiver's map, so both are references.
        v.visit_object(fb.own_desc_receiver);
        v.visit(fb.own_desc_value);
    }
}

namespace {

struct OpInfo {
    const char* name;
    int operand_bytes;
    char kind;  // '-' none, 'r' register(s), 'k' constant, 'o' jump offset, 'i' i8 immediate
};

const OpInfo& op_info(Op op) {
    static const OpInfo table[] = {
        {"LdaConst", 2, 'k'}, {"LdaZero", 0, '-'}, {"LdaSmi", 1, 'i'},
        {"LdaUndefined", 0, '-'}, {"LdaNull", 0, '-'}, {"LdaTrue", 0, '-'}, {"LdaFalse", 0, '-'},
        {"LdaThis", 0, '-'},
        {"Ldar", 1, 'r'}, {"Star", 1, 'r'}, {"Mov", 2, 'r'},
        {"LdaTdz", 0, '-'}, {"LdarChecked", 3, 'l'}, {"StarChecked", 3, 'l'},
        {"Add", 1, 'r'}, {"Sub", 1, 'r'}, {"Mul", 1, 'r'}, {"Div", 1, 'r'},
        {"Mod", 1, 'r'}, {"Exp", 1, 'r'},
        {"BitAnd", 1, 'r'}, {"BitOr", 1, 'r'}, {"BitXor", 1, 'r'},
        {"Shl", 1, 'r'}, {"Shr", 1, 'r'}, {"Sar", 1, 'r'},
        {"TestEq", 1, 'r'}, {"TestNe", 1, 'r'}, {"TestStrictEq", 1, 'r'}, {"TestStrictNe", 1, 'r'},
        {"TestLt", 1, 'r'}, {"TestGt", 1, 'r'}, {"TestLe", 1, 'r'}, {"TestGe", 1, 'r'},
        {"TestInstanceOf", 1, 'r'}, {"TestIn", 1, 'r'},
        {"Neg", 0, '-'}, {"LogicalNot", 0, '-'}, {"BitNot", 0, '-'}, {"TypeOf", 0, '-'},
        {"ToNumber", 0, '-'}, {"ToNumeric", 0, '-'}, {"Inc", 0, '-'}, {"ToTemplateString", 0, '-'}, {"ToPropertyKey", 0, '-'}, {"CheckObjectCoercible", 0, '-'}, {"Dec", 0, '-'},
        {"LdaLookup", 2, 'n'}, {"LdaLookupTypeof", 2, 'n'}, {"StaLookup", 2, 'n'},
        {"CheckLookupResolvable", 2, 'n'}, {"StaLookupChecked", 3, 'l'},
        {"LdaEnv", 2, 'n'}, {"StaEnv", 2, 'n'}, {"StaEnvInit", 2, 'n'},
        {"LdaEnvSlot", 3, 'e'}, {"StaEnvSlot", 3, 'e'}, {"StaEnvSlotInit", 3, 'e'},
        {"BindEnvLocals", 0, '-'},
        {"EnterLoopEnv", 2, 'z'}, {"AdvanceLoopEnv", 2, 'z'}, {"ExitLoopEnv", 0, '-'},
        {"SaveEnv", 0, '-'}, {"RestoreEnv", 0, '-'}, {"PopEnvSave", 0, '-'},
        {"GetIterator", 1, 'r'}, {"IteratorNextOrJump", 4, 'j'}, {"IteratorClose", 2, 'C'},
        {"CreateForInKeys", 1, 'r'},
        {"JumpIfNotNullish", 2, 'o'}, {"JumpIfNullish", 2, 'o'}, {"JumpIfNotUndefined", 2, 'o'},
        {"CreateClosure", 2, 'z'},
        {"DeclareFunction", 2, 'z'},
        {"EvalAst", 2, 'z'},
        {"CopyRestProperties", 2, 'r'},
        {"CreateRestArray", 1, 'A'},
        {"Call", 5, 'c'}, {"CallResolved", 6, 'v'}, {"Construct", 5, 'c'},
        {"CallSpread", 5, 'w'}, {"ConstructSpread", 4, 'W'}, {"SpreadInto", 2, 'r'}, {"ObjectSpreadInto", 1, 'r'}, {"HasPrivate", 2, 'n'},
        {"LdaEngineHelper", 1, 'E'},
        {"CreateRegExp", 4, 'X'},
        {"GetSuper", 2, 'n'}, {"ResolveSuperBase", 1, 'r'}, {"SetSuper", 3, 'l'},
        {"GetSuperKeyed", 1, 'r'}, {"SetSuperKeyed", 2, 'r'}, {"SuperCall", 2, 'S'},
        {"GetNamed", 5, 'g'}, {"SetNamed", 5, 'g'},
        {"GetPrivate", 5, 'g'}, {"SetPrivate", 5, 'g'},
        {"GetKeyed", 3, 'f'}, {"SetKeyed", 4, 'x'},
        {"DeleteNamed", 3, 'l'}, {"DeleteKeyed", 1, 'r'},
        {"DefineOwn", 5, 'g'}, {"DefineElement", 2, 'r'},
        {"ToPropertyKeyStrict", 0, '-'}, {"DefineOwnKeyed", 2, 'r'},
        {"FinalizeStaticProperty", 8, 's'}, {"FinalizeComputedProperty", 4, 'p'},
        {"SetFunctionNameIfUnnamed", 2, 'n'},
        {"CreateObject", 2, 'h'}, {"CreateArray", 2, 'h'},
        {"Jump", 2, 'o'}, {"JumpIfTrue", 2, 'o'}, {"JumpIfFalse", 2, 'o'},
        {"Return", 0, '-'}, {"Throw", 0, '-'}, {"ReraiseGeneratorReturn", 0, '-'},
        {"LdarStar", 2, 'r'}, {"LdaSmiStar", 2, 'I'},
        {"LdaConstStar", 3, 'K'}, {"LdaZeroStar", 1, 'r'},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<size_t>(Op::kCount),
                  "op_info table out of sync with Op enum");
    return table[static_cast<uint8_t>(op)];
}

}

int op_operand_bytes(Op op) { return op_info(op).operand_bytes; }
char op_operand_kind(Op op) { return op_info(op).kind; }

#ifdef QUANTA_VALIDATE_BYTECODE
void validate_chunk_registers(const BytecodeChunk& chunk, const std::string& name) {
    const uint32_t limit = chunk.register_count;
    auto bad = [&](size_t pc, const char* op, const char* what, unsigned reg) {
        std::fprintf(stderr,
            "[bytecode] %s at pc %zu in '%s': %s r%u but the chunk declares %u registers\n",
            op, pc, name.empty() ? "<anonymous>" : name.c_str(), what, reg, limit);
        std::abort();
    };

    size_t pc = 0;
    while (pc < chunk.code.size()) {
        Op op = static_cast<Op>(chunk.code[pc]);
        if (op >= Op::kCount) {
            std::fprintf(stderr, "[bytecode] invalid opcode %d at pc %zu in '%s'\n",
                         static_cast<int>(chunk.code[pc]), pc,
                         name.empty() ? "<anonymous>" : name.c_str());
            std::abort();
        }
        const OpInfo& info = op_info(op);
        const size_t operand_pc = pc + 1;
        // Which operand bytes name a register, per kind -- the same decoding
        // disassemble_chunk does below, kept beside it so the two stay in step.
        auto reg_at = [&](size_t i) { return static_cast<unsigned>(chunk.code[operand_pc + i]); };
        auto check = [&](size_t i, const char* what) {
            if (reg_at(i) >= limit) bad(pc, info.name, what, reg_at(i));
        };
        // An argument list occupies a run of registers starting at `first`.
        // An empty list names the register the run would have started at and
        // never reads it, so that operand may sit one past the end.
        auto check_run = [&](size_t first_i, size_t count_i) {
            const unsigned count = reg_at(count_i);
            if (count == 0) return;
            const unsigned first = reg_at(first_i);
            if (first + count > limit)
                bad(pc, info.name, "argument list ends past", first + count - 1);
        };

        switch (info.kind) {
            case 'r': for (int i = 0; i < info.operand_bytes; i++) check(i, "reads"); break;
            case 'S': check_run(0, 1); break;
            case 'c': check(0, "calls"); check_run(1, 2); break;
            case 'v': check(0, "calls"); check(1, "receiver"); check_run(2, 3); break;
            case 'w': check(0, "calls"); check(1, "receiver"); check(2, "spread array"); break;
            case 'W': check(0, "constructs"); check(1, "spread array"); break;
            case 'g': case 'f': case 'l': case 'm': case 's': check(0, "receiver"); break;
            case 'C': check(0, "closes the iterator in"); break;
            case 'x': case 'j': check(0, "reads"); check(1, "reads"); break;
            case 'I': check(1, "stores into"); break;
            case 'K': check(2, "stores into"); break;
            case 'p': check(0, "reads"); check(1, "key"); check(2, "raw key"); break;
            default: break;  // no register operands
        }
        pc = operand_pc + info.operand_bytes;
    }
}
#endif

std::string disassemble_chunk(const BytecodeChunk& chunk, const std::string& name) {
    std::ostringstream out;
    out << "== " << (name.empty() ? "<anonymous>" : name) << " ==  "
        << chunk.code.size() << " bytes, " << chunk.register_count << " registers ("
        << static_cast<int>(chunk.parameter_count) << " params), "
        << chunk.constants.size() << " constants";
    // Whether the frame gets an Environment of its own is what decides which
    // of the two call paths every call to this chunk takes (Function::call's
    // register-mode gate), so it belongs in the header rather than having to
    // be inferred from the presence of an Op::LdaEnv further down.
    if (chunk.env_mode) {
        out << ", env_mode";
        if (const auto* e = chunk.env.get()) {
            if (!e->env_params.empty() || !e->env_locals.empty())
                out << " (" << e->env_params.size() << " env params, "
                    << e->env_locals.size() << " env locals)";
        }
    }
    out << "\n";

    size_t pc = 0;
    while (pc < chunk.code.size()) {
        Op op = static_cast<Op>(chunk.code[pc]);
        if (op >= Op::kCount) {
            out << "  " << pc << ": <invalid " << static_cast<int>(chunk.code[pc]) << ">\n";
            break;
        }
        const OpInfo& info = op_info(op);
        out << "  " << pc << ": " << info.name;
        size_t operand_pc = pc + 1;
        switch (info.kind) {
            case 'r':
                for (int i = 0; i < info.operand_bytes; i++) {
                    out << " r" << static_cast<int>(chunk.code[operand_pc + i]);
                }
                break;
            case 'k': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " [" << idx << "] ; " << chunk.constants[idx].to_string();
                break;
            }
            case 'o': {
                uint16_t raw = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                int16_t off = static_cast<int16_t>(raw);
                out << " -> " << (operand_pc + 2 + off);
                break;
            }
            case 'i':
                out << " #" << static_cast<int>(static_cast<int8_t>(chunk.code[operand_pc]));
                break;
            case 'I':
                out << " #" << static_cast<int>(static_cast<int8_t>(chunk.code[operand_pc]))
                    << " -> r" << static_cast<int>(chunk.code[operand_pc + 1]);
                break;
            case 'K': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " [" << idx << "] -> r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " ; " << chunk.constants[idx].to_string();
                break;
            }
            case 'S':
                out << " args=r" << static_cast<int>(chunk.code[operand_pc])
                    << " argc=" << static_cast<int>(chunk.code[operand_pc + 1]);
                break;
            case 'n': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " '" << chunk.name_at(idx) << "'";
                break;
            }
            case 'E':
                out << " " << EngineHelper::slot_name(
                    static_cast<EngineHelper::Kind>(chunk.code[operand_pc]));
                break;
            case 'c': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " args=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " argc=" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " '" << chunk.name_at(idx) << "'";
                break;
            }
            case 'v': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 4]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 5]) << 8);
                out << " func=r" << static_cast<int>(chunk.code[operand_pc])
                    << " this=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " args=r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " argc=" << static_cast<int>(chunk.code[operand_pc + 3])
                    << " '" << chunk.name_at(name_idx) << "'";
                break;
            }
            case 'w': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " func=r" << static_cast<int>(chunk.code[operand_pc])
                    << " this=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " args[]=r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " '" << chunk.name_at(name_idx) << "'";
                break;
            }
            case 'W': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 2]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 3]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " args[]=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " '" << chunk.name_at(name_idx) << "'";
                break;
            }
            case 'g': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                uint16_t fb_idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                                  (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.name_at(name_idx) << "'"
                    << " fb=" << fb_idx;
                break;
            }
            case 'h': {
                uint16_t n = static_cast<uint16_t>(chunk.code[operand_pc]) |
                             (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " n=" << n;
                break;
            }
            case 'f': {
                uint16_t fb_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                   (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " fb=" << fb_idx;
                break;
            }
            case 'x': {
                uint16_t fb_idx = static_cast<uint16_t>(chunk.code[operand_pc + 2]) |
                                   (static_cast<uint16_t>(chunk.code[operand_pc + 3]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " fb=" << fb_idx;
                break;
            }
            case 'X': {
                uint16_t p_idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                                 (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                uint16_t f_idx = static_cast<uint16_t>(chunk.code[operand_pc + 2]) |
                                 (static_cast<uint16_t>(chunk.code[operand_pc + 3]) << 8);
                out << " /" << chunk.name_at(p_idx) << "/" << chunk.name_at(f_idx);
                break;
            }
            case 'l': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.name_at(name_idx) << "'";
                break;
            }
            case 'e': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                out << " slot" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.name_at(name_idx) << "'";
                break;
            }
            case 'z': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " [" << idx << "]";
                break;
            }
            case 'm': {
                uint16_t key_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                uint16_t disp_idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                                     (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.name_at(key_idx) << "'"
                    << " '" << chunk.name_at(disp_idx) << "'"
                    << " kind=" << static_cast<int>(chunk.code[operand_pc + 5]);
                break;
            }
            case 's': {
                uint16_t key_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                uint16_t disp_idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                                     (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                uint16_t fb_idx = static_cast<uint16_t>(chunk.code[operand_pc + 6]) |
                                   (static_cast<uint16_t>(chunk.code[operand_pc + 7]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.name_at(key_idx) << "'"
                    << " '" << chunk.name_at(disp_idx) << "'"
                    << " kind=" << static_cast<int>(chunk.code[operand_pc + 5])
                    << " fb=" << fb_idx;
                break;
            }
            case 'p': {
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " key=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " raw=r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " kind=" << static_cast<int>(chunk.code[operand_pc + 3]);
                break;
            }
            case 'C':
                // Second operand is a close mode, not a register.
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " mode=" << static_cast<int>(chunk.code[operand_pc + 1]);
                break;
            case 'A':
                // Operand indexes the incoming argument list, not the registers.
                out << " args[" << static_cast<int>(chunk.code[operand_pc]) << "..]";
                break;
            case 'j': {
                uint16_t raw = static_cast<uint16_t>(chunk.code[operand_pc + 2]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 3]) << 8);
                int16_t off = static_cast<int16_t>(raw);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " -> " << (operand_pc + 4 + off);
                break;
            }
            default:
                break;
        }
        out << "\n";
        pc = operand_pc + info.operand_bytes;
    }
    return out.str();
}

}
