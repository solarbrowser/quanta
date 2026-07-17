/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/vm/Bytecode.h"
#include "quanta/core/gc/Visitor.h"
#include <sstream>

namespace Quanta {

void BytecodeChunk::trace(Visitor& v) const {
    for (const auto& c : constants) {
        v.visit(c);
    }
    // feedback's Shape* fields need no tracing (immortal, not a GC cell), but
    // the prototype-chain read cache's holder/prototype are real cells.
    for (const auto& fb : feedback) {
        for (uint8_t i = 0; i < fb.proto_count; i++) {
            v.visit_object(fb.proto_entries[i].holder);
            v.visit_object(fb.proto_entries[i].prototype);
        }
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
        {"BindEnvLocals", 0, '-'},
        {"EnterLoopEnv", 2, 'z'}, {"AdvanceLoopEnv", 2, 'z'}, {"ExitLoopEnv", 0, '-'},
        {"SaveEnv", 0, '-'}, {"RestoreEnv", 0, '-'}, {"PopEnvSave", 0, '-'},
        {"GetIterator", 1, 'r'}, {"IteratorNextOrJump", 4, 'j'}, {"IteratorClose", 2, 'r'},
        {"CreateForInKeys", 0, '-'},
        {"JumpIfNotNullish", 2, 'o'}, {"JumpIfNullish", 2, 'o'}, {"JumpIfNotUndefined", 2, 'o'},
        {"CreateClosure", 2, 'z'},
        {"DestructureBind", 2, 'z'},
        {"CreateRestArray", 1, 'r'},
        {"Call", 5, 'c'}, {"CallResolved", 6, 'v'}, {"Construct", 5, 'c'},
        {"GetNamed", 5, 'g'}, {"SetNamed", 5, 'g'},
        {"GetPrivate", 5, 'g'}, {"SetPrivate", 5, 'g'},
        {"GetKeyed", 1, 'r'}, {"SetKeyed", 2, 'r'},
        {"DeleteNamed", 3, 'l'}, {"DeleteKeyed", 1, 'r'},
        {"DefineOwn", 3, 'l'}, {"DefineElement", 2, 'r'},
        {"ToPropertyKeyStrict", 0, '-'}, {"DefineOwnKeyed", 2, 'r'},
        {"FinalizeStaticProperty", 6, 'm'}, {"FinalizeComputedProperty", 4, 'p'},
        {"SetFunctionNameIfUnnamed", 2, 'n'},
        {"CreateObject", 2, 'h'}, {"CreateArray", 2, 'h'},
        {"Jump", 2, 'o'}, {"JumpIfTrue", 2, 'o'}, {"JumpIfFalse", 2, 'o'},
        {"Return", 0, '-'}, {"Throw", 0, '-'}, {"ReraiseGeneratorReturn", 0, '-'},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<size_t>(Op::kCount),
                  "op_info table out of sync with Op enum");
    return table[static_cast<uint8_t>(op)];
}

}

std::string disassemble_chunk(const BytecodeChunk& chunk, const std::string& name) {
    std::ostringstream out;
    out << "== " << (name.empty() ? "<anonymous>" : name) << " ==  "
        << chunk.code.size() << " bytes, " << chunk.register_count << " registers ("
        << static_cast<int>(chunk.parameter_count) << " params), "
        << chunk.constants.size() << " constants\n";

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
            case 'n': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " '" << chunk.names[idx] << "'";
                break;
            }
            case 'c': {
                uint16_t idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                               (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " args=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " argc=" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " '" << chunk.names[idx] << "'";
                break;
            }
            case 'v': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 4]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 5]) << 8);
                out << " func=r" << static_cast<int>(chunk.code[operand_pc])
                    << " this=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " args=r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " argc=" << static_cast<int>(chunk.code[operand_pc + 3])
                    << " '" << chunk.names[name_idx] << "'";
                break;
            }
            case 'g': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                uint16_t fb_idx = static_cast<uint16_t>(chunk.code[operand_pc + 3]) |
                                  (static_cast<uint16_t>(chunk.code[operand_pc + 4]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.names[name_idx] << "'"
                    << " fb=" << fb_idx;
                break;
            }
            case 'h': {
                uint16_t n = static_cast<uint16_t>(chunk.code[operand_pc]) |
                             (static_cast<uint16_t>(chunk.code[operand_pc + 1]) << 8);
                out << " n=" << n;
                break;
            }
            case 'l': {
                uint16_t name_idx = static_cast<uint16_t>(chunk.code[operand_pc + 1]) |
                                    (static_cast<uint16_t>(chunk.code[operand_pc + 2]) << 8);
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " '" << chunk.names[name_idx] << "'";
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
                    << " '" << chunk.names[key_idx] << "'"
                    << " '" << chunk.names[disp_idx] << "'"
                    << " kind=" << static_cast<int>(chunk.code[operand_pc + 5]);
                break;
            }
            case 'p': {
                out << " r" << static_cast<int>(chunk.code[operand_pc])
                    << " key=r" << static_cast<int>(chunk.code[operand_pc + 1])
                    << " raw=r" << static_cast<int>(chunk.code[operand_pc + 2])
                    << " kind=" << static_cast<int>(chunk.code[operand_pc + 3]);
                break;
            }
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
