// -*- C++ -*-
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Optimizing tier (optimizing-tier-plan.md). Included by Jit.inc inside namespace jit: like the
// baseline JIT it is built on the interpreter's own Frame and handlers, so it cannot live in a
// separate translation unit.
//
// Model, in one paragraph. A chunk is turned into SSA over the bytecode registers (plus the
// accumulator as one more variable), speculating that arithmetic operands are numbers. The Frame's
// register bank is *memory the optimized code does not keep current*: values live in machine
// registers, and a variable is written back to the bank only when something outside the optimized
// code could look at it -- a step into the interpreter (any opcode the tier does not translate), a
// guard that fails (deoptimization), the end of the function. The bookkeeping that makes this cheap
// is the MEM sentinel: a variable whose definition is MEM equals the bank's contents, so there is
// nothing to write back for it, and a later read just loads it. A step is therefore a full barrier:
// flush the dirty variables, run the interpreter's own handler on the same Frame, and forget
// everything (every variable becomes MEM again). That is what lets the tier cover every opcode on
// day one without a second implementation of any of them.
//
// Deoptimization is "write the dirty variables back, set Frame::pc to the instruction that was
// about to run, return status 2": the interpreter resumes at that pc, re-executing the whole
// instruction. So every guard sits before its instruction's side effects.

namespace opt {

constexpr int32_t kMem = -1;      // a variable whose value is the register bank's content
constexpr int32_t kUnset = -2;    // builder-internal: not looked up yet in this block
constexpr int32_t kNoRepl = -3;   // a node that has not been replaced by another

enum class Rep : uint8_t { None, Tagged, F64, I32, Bool, Ptr };

enum class K : uint8_t {
    ConstTagged, ConstF64, ConstBool, ConstI32,
    LoadReg, LoadAcc,
    Phi,
    // Pseudo-ops the builder emits before representations are known; lowering replaces them.
    ToNum,        // any -> F64   (guards unless the input is already F64)
    ToTagged,     // any -> Tagged
    ToBool,       // any -> Bool
    // Lowered forms.
    CheckNum,     // Tagged -> F64, deopt unless a number
    BoxF64, BoxBool,
    ToI32,        // F64 -> I32 (ToInt32), deopt on the one case the hardware conversion cannot decide
    I32ToF64, U32ToF64,
    AddF, SubF, MulF, DivF, NegF,
    AndI, OrI, XorI, ShlI, SarI, ShrI, NotI,
    LtF, GtF, LeF, GeF, EqF, NeF,
    EqTag, NeTag,         // identity of two Tagged values (bits)
    NotBool,
    Truthy,               // Tagged -> Bool
    TruthyF,              // F64 -> Bool
    NotTdz,               // Tagged -> Tagged, deopt if it is the TDZ marker
    StoreReg, StoreAcc,   // write a Tagged value to the register bank / Frame::acc
    Step,                 // run the interpreter's handler for the instruction at aux; I32 result (next pc)
    StepExit,             // leave unless the step result equals the fall-through pc (imm)
    Poll,                 // GC safepoint poll
    // Objects. Ptr is a raw Object* (the payload of an object-tagged value); every access is guarded.
    CheckObj,             // Tagged -> Ptr, deopt unless an object
    CheckShape,           // guard: a's shape word == imm and it has no descriptor overrides
    LoadSlot,             // Ptr a -> Tagged, shape slot aux
    StoreSlot,            // Ptr a, Tagged b -> shape slot aux, with the collector's barrier when b can hold a cell
    CheckDense,           // guard: a is a verified dense Array
    ToIndex,              // F64 a -> I32, deopt unless a non-negative int32
    LoadElem,             // Ptr a, I32 b -> Tagged, deopt when b is out of range
    StoreElem,            // Ptr a, I32 b, Tagged c, deopt when b is out of range
    ArrayLen,             // Ptr a (a dense Array) -> F64
    ModF,                 // F64 a % b
    LoadThis,             // the frame's `this`, deopt unless it is resolved and needs no super lookup
    LoadEnv,              // Tagged, slot aux of the current lexical environment (key imm); deopt on a miss or a hole
    StoreEnv,             // Tagged a -> that slot, deopt on a miss, a hole or a constant; barrier
    NonNegI32,            // I32 a -> a, deopt when negative
    CheckFn,              // Tagged a: deopt unless it is a function whose executable is imm (aux 1: unless it is the function imm)
    CheckProto,           // Ptr a: deopt unless its prototype is imm
    CheckEpoch,           // deopt unless the global prototype epoch (aux 0) or descriptor epoch (aux 1) is imm
    LoadHolder,           // Tagged, shape slot aux of the object at imm (a prototype that holds a method)
    LoadPoly,             // Tagged, a Ptr: the slot of the entry in polys[aux] whose shape word the object has (deopt if none)
    CheckNotNullish,      // Tagged a -> a, deopt when it is undefined or null
    IsFn,                 // Tagged a -> Bool: a function whose executable is imm (aux 1: the function imm itself)
    Trap,                 // always deoptimizes: the code after it has never run
    CheckBits,            // Tagged a: deopt unless it is exactly the word imm (an object known by identity)
    MathF,                // F64 a [, F64 b]: the Math function aux (kMathFns)
    CallNative,           // Tagged a (this), b, c (arguments): calls the native in the NativeDesc at imm, or deopts
    NewLiteral,           // Tagged: a fresh object literal of the learned template at imm (a LiteralTemplate*), or deopt
    NewObject,            // Tagged a (the constructor): a fresh object of the shape described by imm (NewDesc*), or deopt
    IsNullish,            // Tagged a -> Bool: undefined or null
    LoadLookup,           // Tagged, a name's cached binding (cache entry: imm; aux bits 0-1 the form, bit 8 set when imm is an offset from the frame's cache)
};

struct Node {
    K op;
    Rep rep = Rep::None;
    int32_t a = -1, b = -1, c = -1;
    uint32_t aux = 0;        // register index / pc
    uint64_t imm = 0;
    int32_t state = -1;      // deopt snapshot, for nodes that can deoptimize
    int32_t blk = -1;
    std::vector<int32_t> ins; // Phi operands (one per predecessor of the block, in order)
    bool dead = false;
    bool light = false;       // Step: only the registers the instruction reads were written back, and values stay live across it
};

enum class T : uint8_t { None, Jump, Branch, Return, StepBranch, Exit, Bail };

struct Term {
    T kind = T::None;
    int32_t cond = -1;       // Branch: Bool node; StepBranch: the Step node; Return: Tagged node
    int32_t t = -1, f = -1;  // successor blocks (Jump uses t)
    uint32_t tpc = 0, fpc = 0;   // StepBranch: pcs the step may continue at; Bail: the pc to resume at
};

struct State {
    int32_t func = 0;           // the function the pc belongs to (the innermost one, when inlined)
    uint32_t pc = 0;
    int32_t edge_var = -1;      // set when the guard is a phi's check on an incoming edge: the variable
    std::vector<std::pair<uint32_t, int32_t>> defs;   // (variable, node) for every dirty variable
};

struct Block {
    uint32_t start_pc = 0, end_pc = 0;   // [start, end) in bytecode; both 0 for synthetic blocks
    std::vector<int32_t> preds, succs;
    std::vector<int32_t> nodes;          // phis first
    Term term;
    std::vector<int32_t> def;            // per variable at the end of the block (kUnset until looked up)
    std::vector<std::pair<uint32_t, int32_t>> incomplete;   // (variable, phi) waiting for the block to seal
    bool sealed = false, built = false, reachable = false, loop_header = false, synthetic = false;
    int32_t filled_preds = 0;
    int32_t osr_pc = -1;                 // synthetic entry block: the bytecode pc it enters at
    int32_t exit_pc = -1;                // synthetic exit block: leaves the region, resuming at this pc
    int32_t first_insn = -1, last_insn = -1;   // [first, last] indices into the function's instructions
    int32_t func = 0;                    // which function's bytecode the block is made of
    bool cold = false;                   // a block of an inlined function that has never run: it only deoptimizes
    int32_t poly_kind = 0, poly_site = -1, poly_t = -1;   // 1: tests callee poly_t of a polymorphic call, 2: sets that callee up
    int32_t ret_to = -1;                 // a block of an inlined function that returns: where it goes
};

struct Insn { uint32_t pc; uint8_t op; uint8_t len; };

// One function whose bytecode is part of the graph: the root, or a callee inlined into it. Variables are
// numbered across all of them, each function's registers followed by its accumulator.
struct Func {
    const BytecodeChunk* chunk = nullptr;
    int32_t parent = -1;                 // the function it was inlined into
    uint32_t var_base = 0, nregs = 0, acc_var = 0;
    std::vector<Insn> insns;
    std::vector<int32_t> insn_at;
    std::vector<int32_t> block_at;       // pc -> block id (leaders only)
    std::vector<uint64_t> live_before;   // per instruction: the variables the interpreter may still read (local numbering)
    uint32_t live_words = 0;
    // Inlined instance only.
    uint32_t call_pc = 0, call_len = 0;  // the call instruction in the parent
    int32_t cont_block = -1;             // the parent's block that starts at the return pc
    int32_t entry_block = -1;
    const void* exe = nullptr;           // the executable the call site was seen to reach (guarded on entry)
    bool no_light = false;               // the function makes an arguments object, which can alias its registers: its steps write everything back
    bool ctor = false;                   // a constructor run for `new`: private to the new object, so a deoptimization leaves through the `new` itself
    int32_t plan = -1;                   // its entry in Graph::ctors
    bool sloppy_this = false;            // reads a sloppy `this`: only an object may reach it as it is
    Function* fnp = nullptr;             // the function itself
    bool by_identity = false;            // the callee is recognized by the function, not its executable (its caches belong to it)
    BytecodeChunk::LookupCacheEntry* lookup = nullptr;   // the name cache its instructions use (the root's is the frame's)
    int32_t this_var = -1, fn_var = -1;  // variables holding the callee and its `this` (deoptimization rebuilds the frame from them)
    bool leaf = false;                   // a body whose guards can deoptimize to the call itself: it has done nothing yet
    int32_t entry_state = -1;            // that state
    uint32_t params = 0;
    std::vector<std::pair<uint32_t, Function*>> cand;   // call pc -> what the feedback says it calls (and the tier may inline)
    std::vector<std::pair<uint32_t, int32_t>> sites;          // call pc -> the inlined instance
    struct PolySite {                                         // a call with several callees, told apart by identity checks
        uint32_t pc = 0;
        Insn call{};
        std::vector<int32_t> callees;                         // the instances, in the order they are tried
        int32_t fv = -1;                                      // the callee value, read once by the call block
    };
    std::vector<PolySite> polys;
};

struct Graph {
    const BytecodeChunk* chunk = nullptr;     // the root's
    const Frame* frame = nullptr;         // the activation the compile was triggered from, for peeking at register contents
    uint32_t entry_pc = 0;                // the requested entry (the one the frame is positioned at)
    uint32_t nvars = 0, acc_var = 0;          // all variables; the root's accumulator
    std::vector<Func> funcs;
    std::vector<Node> n;
    std::vector<Block> b;
    struct CtorPlan {
        std::vector<std::pair<uint32_t, uint32_t>> stores;
        uint64_t proto_epoch = 0;
        JitCode::OptUnit::NewDesc* desc = nullptr;
        // A constructor that only forwards to `this.<name>.apply(this, arguments)` (Prototype.js's Class.create):
        // the body that is inlined is that method's, found on the prototype chain and checked again at run time.
        Function* inner = nullptr;
        Object* holder = nullptr;
        uint32_t holder_slot = 0;
        uint64_t inner_bits = 0, desc_epoch = 0;
    };
    std::vector<CtorPlan> ctors;
    std::vector<std::unique_ptr<JitCode::OptUnit::NewDesc>> descs;
    std::vector<std::unique_ptr<JitCode::OptUnit::NativeDesc>> natives;
    struct Poly { uint8_t n = 0; uint64_t word[4] = {}; uint32_t slot[4] = {}; };
    std::vector<Poly> polys;             // the shapes a polymorphic property read serves
    std::vector<State> states;
    std::vector<int32_t> entries;        // synthetic entry blocks
    std::vector<int32_t> rpo;
    std::vector<int32_t> repl;           // node replacement (forward pointers; may point at kMem)
    const char* why = nullptr;
    // Inlining: the arena words and frames the deepest chain of inlined callees needs once a deoptimization
    // rebuilds them, the strictness they were checked against, and the call site whose callee needed a step.
    bool fn_entry = false;                // the unit is for the whole function, entered at its first instruction
    uint32_t inl_words = 0, inl_depth = 0, inl_count = 0;
    bool inl_strict = false;
    std::pair<const BytecodeChunk*, uint32_t> failed_site{nullptr, 0};
};

inline const char* kname(K k) {
    static const char* names[] = {
        "ConstTagged", "ConstF64", "ConstBool", "ConstI32", "LoadReg", "LoadAcc", "Phi", "ToNum", "ToTagged", "ToBool", "CheckNum", "BoxF64", "BoxBool", "ToI32", "I32ToF64", "U32ToF64", "AddF", "SubF", "MulF", "DivF", "NegF", "AndI", "OrI", "XorI", "ShlI", "SarI", "ShrI", "NotI", "LtF", "GtF", "LeF", "GeF", "EqF", "NeF", "EqTag", "NeTag", "NotBool", "Truthy", "TruthyF", "NotTdz", "StoreReg", "StoreAcc", "Step", "StepExit", "Poll", "CheckObj", "CheckShape", "LoadSlot", "StoreSlot", "CheckDense", "ToIndex", "LoadElem", "StoreElem", "ArrayLen", "ModF", "LoadThis", "LoadEnv", "StoreEnv", "NonNegI32", "CheckFn", "CheckProto", "CheckEpoch", "LoadHolder", "LoadPoly", "CheckNotNullish", "IsFn", "Trap", "CheckBits", "MathF", "CallNative", "NewLiteral", "NewObject", "IsNullish", "LoadLookup"};
    return names[static_cast<int>(k)];
}
inline const char* rname(Rep r) {
    switch (r) { case Rep::Tagged: return "T"; case Rep::F64: return "f"; case Rep::I32: return "i"; case Rep::Bool: return "b"; case Rep::Ptr: return "p"; default: return "-"; }
}

inline bool is_pure_removable(K k) {
    switch (k) {
    case K::StoreReg: case K::StoreAcc: case K::Step: case K::StepExit: case K::Poll:
    case K::CheckNum: case K::ToI32: case K::NotTdz:      // guards stay even when their value is unused
    case K::StoreSlot: case K::StoreElem: case K::CheckObj: case K::CheckShape: case K::CheckDense:
    case K::ToIndex: case K::LoadElem: case K::LoadThis: case K::LoadEnv: case K::StoreEnv: case K::NonNegI32: case K::CheckFn: case K::CheckProto: case K::CheckEpoch: case K::Trap: case K::CheckNotNullish:
    case K::CallNative: case K::CheckBits:
        return false;
    default: return true;
    }
}

constexpr uint64_t kTdzBits = 0x7FFC000000000000ull;      // QUIET_NAN | TAG_VM_TDZ
constexpr uint32_t kNoPc = 0xFFFFFFFFu;

// What deoptimization taught about a chunk: instructions whose speculation kept failing (compiled as
// steps from now on) and loop-carried variables that turned out not to be numbers (kept tagged).
struct Avoid {
    std::set<uint32_t> pcs;
    std::set<std::pair<uint32_t, uint32_t>> phis;     // (loop header pc, variable)
    std::map<uint64_t, uint32_t> fails;               // site -> failures so far
    std::set<std::pair<const BytecodeChunk*, uint32_t>> no_inline;   // call sites (chunk, pc) that are not worth inlining
    uint32_t recompiles = 0;
};
inline std::map<const BytecodeChunk*, Avoid>& avoid_table() { static std::map<const BytecodeChunk*, Avoid> t; return t; }

// What an optimized unit's entry function returns.
constexpr uint64_t kStatusFinished = 0, kStatusRethrow = 1, kStatusResume = 2, kStatusDeopt = 3;

struct Config {
    bool init = false;
    bool enabled = true;       // on unless QUANTA_OPT=0
    bool log = false;          // QUANTA_OPT_LOG
    bool dump = false;         // QUANTA_OPT_DUMP: print the IR of every chunk an offer is made for
    uint32_t max_insns = 4000;
    uint32_t stress = 0;              // QUANTA_OPT_DEOPT_STRESS=N: every Nth guard evaluation deoptimizes (testing)
    uint32_t skip = 0, limit = 0xFFFFFFFFu, seen = 0;   // QUANTA_OPT_SKIP / QUANTA_OPT_LIMIT: bisect which compiles are allowed
    uint32_t max_deopts = 40;         // guard failures before a loop is given up on
    bool entry = true;                // QUANTA_OPT_ENTRY=0 turns the function-entry units off
    bool cold = true;                 // QUANTA_OPT_COLD=0 keeps never-executed blocks in the region
    bool light = false;               // QUANTA_OPT_LIGHT=1: steps write back only what they read (measured: no gain so far)
    bool allow_calls = false;         // QUANTA_OPT_CALLS=1: function-entry units may keep calls as steps
    uint32_t poly_max = 4;            // QUANTA_OPT_POLY=n: most callees one call site may have and still be inlined (1: monomorphic only)
    bool natives = true;              // QUANTA_OPT_NATIVES=0 keeps native calls as steps
    bool inl = true;                  // QUANTA_OPT_INLINE=0 turns call inlining off
    uint32_t entry_min_insns = 24;    // smallest loop-free body a function-entry unit is built for
    uint32_t inline_insns = 120;      // largest callee (decoded instructions)
    uint32_t inline_total = 500;      // all the callees of one unit together
    uint32_t inline_depth = 3;
    uint32_t inline_vars = 56;        // variables the inlined callees add (the deopt scratch buffer is 64 words)
    uint32_t entry_arm_at = 64, entry_compile_at = 64;   // QUANTA_OPT_ARM / QUANTA_OPT_PROFILE: calls before a function is watched, and watched before it is compiled
    uint32_t max_step_percent = 12;   // refuse a region in which more than this share of the instructions are plain steps
};
inline Config& config() {
    static Config c;
    if (!c.init) {
        c.init = true;
        if (const char* e = std::getenv("QUANTA_OPT")) c.enabled = e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_LOG")) c.log = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_DUMP")) { c.dump = e[0] && e[0] != '0'; }
        if (const char* e = std::getenv("QUANTA_OPT_MAXINSNS")) c.max_insns = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_DEOPT_STRESS")) { c.stress = static_cast<uint32_t>(std::atoi(e)); c.max_deopts = 0x7FFFFFFF; }
        if (const char* e = std::getenv("QUANTA_OPT_SKIP")) c.skip = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_LIMIT")) c.limit = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_MAXDEOPTS")) c.max_deopts = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_ENTRY")) c.entry = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_ENTRY_MIN")) c.entry_min_insns = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_COLD")) c.cold = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_NATIVES")) c.natives = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_ARM")) c.entry_arm_at = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_PROFILE")) c.entry_compile_at = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_LIGHT")) c.light = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_CALLS")) c.allow_calls = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_POLY")) c.poly_max = static_cast<uint32_t>(std::atoi(e));
        if (const char* e = std::getenv("QUANTA_OPT_INLINE")) c.inl = e[0] && e[0] != '0';
        if (const char* e = std::getenv("QUANTA_OPT_MAXSTEPS")) c.max_step_percent = static_cast<uint32_t>(std::atoi(e));
    }
    return c;
}


class Builder {
public:
    using Site = std::pair<const BytecodeChunk*, uint32_t>;
    // `no_inline`: call sites the caller has found not to be worth (or able to be) inlined, besides what the Avoid table says.
    explicit Builder(Graph& g, const std::set<Site>* no_inline = nullptr) : g_(g), no_inline_(no_inline) {
        auto it = avoid_table().find(g.chunk);
        if (it != avoid_table().end()) avoid_ = &it->second;
    }

    // Decodes, builds the CFG for the region reachable from `entry_pcs`, constructs SSA, picks
    // representations. False (with g.why set) when the chunk is not something the tier takes.
    bool build(const std::vector<uint32_t>& entry_pcs, size_t max_insns);

private:
    Graph& g_;
    const std::set<Site>* no_inline_ = nullptr;
    int32_t cb_ = -1;              // block being built
    int32_t cf_ = 0;               // the function it belongs to
    Func& F() { return g_.funcs[cf_]; }
    const Func& F() const { return g_.funcs[cf_]; }
    int32_t cur_state_ = -1;
    uint32_t cur_pc_ = 0;
    bool state_valid_ = false;

    uint32_t u16(uint32_t at) const { return read_u16(F().chunk->code.data(), at); }
    int16_t i16(uint32_t at) const { return read_i16(F().chunk->code.data(), at); }

    bool fail(const char* why) { if (!g_.why) g_.why = why; return false; }

    // ---- graph plumbing ----
    int32_t new_block(uint32_t start, uint32_t end) {
        Block blk;
        blk.start_pc = start; blk.end_pc = end;
        blk.def.assign(g_.nvars, kUnset);
        g_.b.push_back(std::move(blk));
        return static_cast<int32_t>(g_.b.size() - 1);
    }
    int32_t add_node(int32_t blk, K op, Rep rep, int32_t a = -1, int32_t b = -1, uint32_t aux = 0, uint64_t imm = 0, int32_t c = -1) {
        Node nd;
        nd.op = op; nd.rep = rep; nd.a = a; nd.b = b; nd.c = c; nd.aux = aux; nd.imm = imm; nd.blk = blk;
        g_.n.push_back(std::move(nd));
        g_.repl.push_back(kNoRepl);
        return static_cast<int32_t>(g_.n.size() - 1);
    }
    int32_t emit_in(int32_t blk, K op, Rep rep, int32_t a = -1, int32_t b = -1, uint32_t aux = 0, uint64_t imm = 0, int32_t c = -1) {
        const int32_t id = add_node(blk, op, rep, a, b, aux, imm, c);
        g_.b[blk].nodes.push_back(id);
        return id;
    }
    int32_t emit(K op, Rep rep, int32_t a = -1, int32_t b = -1, uint32_t aux = 0, uint64_t imm = 0, int32_t c = -1) {
        return emit_in(cb_, op, rep, a, b, aux, imm, c);
    }
    int32_t resolve(int32_t id) const {
        while (id >= 0 && g_.repl[id] != kNoRepl) id = g_.repl[id];
        return id;
    }

    // ---- SSA construction (Braun et al.), with kMem as a first-class definition ----
    int32_t read_var(uint32_t v, int32_t blk);
    int32_t read_var_recursive(uint32_t v, int32_t blk);
    int32_t new_phi(uint32_t v, int32_t blk);
    int32_t add_phi_operands(uint32_t v, int32_t phi);
    int32_t try_remove_trivial(int32_t phi);
    void seal(int32_t blk);
    void write_var(uint32_t v, int32_t blk, int32_t def) { g_.b[blk].def[v] = def; }
    // A load of the variable from the register bank, made while the bank still held what it holds now: only then is
    // the variable's value "what is in memory". A light step rewrites the registers it produces, so a load made before
    // one (and copied into another variable since) says nothing about the bank any more -- hence the version.
    static constexpr uint64_t kAnyVersion = ~0ull;
    std::vector<uint64_t> memver_;
    bool is_own_load(uint32_t v, int32_t d) const {
        if (d < 0) return false;
        const Node& nd = g_.n[d];
        if (!((nd.op == K::LoadReg && nd.aux == v) || (nd.op == K::LoadAcc && v == g_.acc_var))) return false;
        return nd.imm == kAnyVersion || nd.imm == (v < memver_.size() ? memver_[v] : 0);
    }
    // A load of the variable itself is the memory state, whichever block made it.
    int32_t norm(uint32_t v, int32_t d) const {
        d = resolve(d);
        return is_own_load(v, d) ? kMem : d;
    }
    // The definition of `v` as an operand: loads it if it is the memory state.
    int32_t value(uint32_t v) {
        int32_t d = resolve(read_var(v, cb_));
        if (d == kMem && v > g_.acc_var) { fail("an inlined variable read before it was set"); return emit(K::ConstTagged, Rep::Tagged, -1, -1, 0, 0); }
        if (d == kMem) {
            d = (v == g_.acc_var) ? emit(K::LoadAcc, Rep::Tagged, -1, -1, 0, memver_[v]) : emit(K::LoadReg, Rep::Tagged, -1, -1, v, memver_[v]);
            write_var(v, cb_, d);
        }
        return d;
    }
    void set(uint32_t v, int32_t d) { write_var(v, cb_, d); state_valid_ = false; }

    // ---- deopt state ----
    int32_t snapshot_at(int32_t blk, int32_t fi, uint32_t pc, int32_t edge_var = -1);
    void compute_liveness();
    // Whether the variable (a global number) may still be read by the interpreter at `pc` of function `fi`.
    bool live_at_f(int32_t fi, uint32_t pc, uint32_t v) const {
        const Func& fn = g_.funcs[fi];
        const uint32_t lv = v - fn.var_base;
        if (pc >= fn.insn_at.size()) return true;      // not a pc of this function: assume it is live
        const int32_t i = fn.insn_at[pc];
        return i < 0 || (fn.live_before[static_cast<size_t>(i) * fn.live_words + (lv >> 6)] >> (lv & 63)) & 1;
    }
    bool live_at(uint32_t pc, uint32_t v) const { return live_at_f(cf_, pc, v); }
    // The variables a deoptimization at `pc` of function `fi` must write back: that function's live ones, and
    // at each enclosing function the ones live where it resumes after the call.
    void state_vars(int32_t fi, uint32_t pc, std::vector<uint32_t>& out) const {
        for (;;) {
            const Func& fn = g_.funcs[fi];
            for (uint32_t l = 0; l <= fn.nregs; l++)
                if (live_at_f(fi, pc, fn.var_base + l)) out.push_back(fn.var_base + l);
            if (fn.parent < 0) break;
            out.push_back(static_cast<uint32_t>(fn.this_var));
            out.push_back(static_cast<uint32_t>(fn.fn_var));
            pc = fn.call_pc + fn.call_len;
            fi = fn.parent;
        }
    }
    int32_t snapshot() {
        if (!state_valid_) { cur_state_ = snapshot_at(cb_, cf_, cur_pc_); state_valid_ = true; }
        return cur_state_;
    }
    // ---- inlining ----
    bool inline_targets(int32_t fi, const Insn& in, std::vector<Function*>& out);
    bool ctor_plan(Function* fn, Graph::CtorPlan& plan, uint32_t& insns, bool make_desc);
    bool block_is_cold(int32_t fi, int32_t blk) const;
    bool native_call(const Insn& in);
    bool numeric_arg(int32_t x) { const Node& nd = g_.n[resolve(x)]; return nd.rep != Rep::Bool && nd.rep != Rep::I32 && nd.op != K::ConstTagged; }
    bool inline_scan(const Function* fn, const BytecodeChunk& c, uint32_t& insns);
    bool expand_inline(int32_t blk, std::vector<int32_t>& added);
    bool inline_enter(const Insn& in, int32_t callee, bool& ended);
    bool inline_define(const Insn& in, int32_t ci, int32_t fv, bool guard);
    bool poly_dispatch(int32_t site, int32_t t, bool& ended);
    int32_t make_instance(int32_t fi, const Insn& call, Function* target, std::vector<int32_t>& added);
    int32_t poly_of(int32_t fi, uint32_t pc) const {
        const auto& ps = g_.funcs[fi].polys;
        for (size_t k = 0; k < ps.size(); k++) if (ps[k].pc == pc) return static_cast<int32_t>(k);
        return -1;
    }
    int32_t site_of(int32_t fi, uint32_t pc) const {
        for (auto& [p, c] : g_.funcs[fi].sites) if (p == pc) return c;
        return -1;
    }
    uint32_t inline_insns_ = 0, inline_vars_ = 0;
    // Whether the register `rcv` is to be taken for a dense array at an indexed access. A loop's entry frame shows what
    // the registers hold; a function entry (and an inlined callee) has no such frame, so it assumes an array and lets a
    // site that proves otherwise deoptimize into the interpreter, which then keeps that site.
    bool dense_array_receiver(uint32_t rcv) const {
        if (cf_ == 0 && g_.frame && !g_.fn_entry) {
            const Value& pv = g_.frame->regs[rcv];
            return pv.is_object() && pv.as_object()->is_verified_dense_array();
        }
        return true;
    }
    void begin_op(uint32_t pc) { cur_pc_ = pc; state_valid_ = false; }
    // A check of one of the global epochs. Its state is a copy marked edge_var -2: failing it only means the world
    // changed since the unit was built, so the unit is rebuilt (the epoch it checks is read again) instead of the
    // site being given up on.
    void epoch_guard(uint32_t which, uint64_t value) {
        const int32_t id = emit(K::CheckEpoch, Rep::None, -1, -1, which, value);
        State s = g_.states[snapshot()];
        s.edge_var = -2;
        g_.states.push_back(std::move(s));
        g_.n[id].state = static_cast<int32_t>(g_.states.size() - 1);
    }

    // ---- typing helpers (builder time) ----
    // The value as an F64, or -1 when it can never be one (a boolean, a non-number constant).
    int32_t num(int32_t x);
    int32_t tagged(int32_t x);
    int32_t boolean(int32_t x);
    int32_t const_of(const Value& v);
    int32_t guarded(K op, Rep rep, int32_t a, int32_t b = -1, uint32_t aux = 0, uint64_t imm = 0, int32_t c = -1) {
        const int32_t id = emit(op, rep, a, b, aux, imm, c);
        g_.n[id].state = snapshot();
        return id;
    }

    void flush_all(int32_t live_pc = -1);
    bool setup_func(int32_t fi, size_t max_insns);
    size_t total_insns_ = 0;
    bool translate_block(int32_t blk);
    // True when handled natively; false means the caller steps the instruction.
    bool translate(const Insn& in, bool& ended);
    bool avoided(uint32_t pc) const { return avoid_ && avoid_->pcs.count(pc); }
    const Avoid* avoid_ = nullptr;
    void step(const Insn& in, bool& ended);

    void finalize();
    void complete_defs();
    bool frozen_ = false;      // SSA construction is over: nothing may create a phi any more
    void materialize_mem_operands();
    void remove_trivial_phis();
    void select_representations();
    void lower();
    void eliminate_dead();
    void fold();
    void cse();
    void optimize() { fold(); cse(); fold(); }
    std::vector<int32_t> compute_rpo();

    // Bytecode registers 0..nregs-1 plus the accumulator.
    uint32_t acc() const { return F().acc_var; }
};


// ===========================================================================
// Register liveness over the whole chunk
// ===========================================================================
//
// What the interpreter may still read of the register file at each instruction. An instruction the
// tier knows the operands of reads exactly those; anything else reads everything, which is what keeps
// the analysis safe -- an opcode's handler is free to look at any register, and a step runs one.
// Used to leave dead temporaries out of phis, out of entry guards and out of deoptimization states.

struct Effects {
    bool read_all = false;
    uint8_t nr = 0, nw = 0;
    uint16_t r[40], w[3];
    void rd(uint32_t v) { if (nr < 40) r[nr++] = static_cast<uint16_t>(v); else read_all = true; }
    void wr(uint32_t v) { w[nw++] = static_cast<uint16_t>(v); }
};

inline Effects op_effects(Op op, const uint8_t* code, uint32_t pc, uint32_t acc, uint32_t nregs) {
    Effects e;
    auto R = [&](uint32_t at) { e.rd(code[at]); };
    auto W = [&](uint32_t at) { e.wr(code[at]); };
    switch (op) {
    case Op::LdaConst: case Op::LdaZero: case Op::LdaSmi: case Op::LdaUndefined: case Op::LdaNull:
    case Op::LdaTrue: case Op::LdaFalse: case Op::LdaThis: case Op::LdaLookup: case Op::LdaEnvSlot:
        e.wr(acc); break;
    case Op::LdaEnvSlotAt: e.wr(acc); break;
    case Op::Ldar: R(pc + 1); e.wr(acc); break;
    case Op::Star: e.rd(acc); W(pc + 1); break;
    case Op::Mov: R(pc + 1); W(pc + 2); break;
    case Op::LdarStar: R(pc + 1); e.wr(acc); W(pc + 2); break;
    case Op::LdaSmiStar: e.wr(acc); W(pc + 2); break;
    case Op::LdaZeroStar: case Op::LdaThisStar: e.wr(acc); W(pc + 1); break;
    case Op::LdaConstStar: case Op::LdaLookupStar: e.wr(acc); W(pc + 3); break;
    case Op::LdaEnvSlotStar: e.wr(acc); W(pc + 4); break;
    case Op::LdarChecked: R(pc + 1); e.wr(acc); break;
    case Op::StarChecked: e.rd(acc); R(pc + 1); W(pc + 1); break;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Exp:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::Sar:
    case Op::TestEq: case Op::TestNe: case Op::TestStrictEq: case Op::TestStrictNe:
    case Op::TestLt: case Op::TestGt: case Op::TestLe: case Op::TestGe:
        R(pc + 1); e.rd(acc); e.wr(acc); break;
    case Op::Neg: case Op::LogicalNot: case Op::BitNot: case Op::ToNumber: case Op::ToNumeric: case Op::Inc: case Op::Dec:
        e.rd(acc); e.wr(acc); break;
    case Op::Jump: break;
    case Op::JumpIfTrue: case Op::JumpIfFalse: case Op::Return: case Op::JumpIfNullish: case Op::JumpIfNotNullish:
    case Op::JumpIfNotUndefined: e.rd(acc); break;
    case Op::GetNamed: R(pc + 1); e.wr(acc); break;
    case Op::GetNamedStar: R(pc + 1); e.wr(acc); W(pc + 6); break;
    case Op::SetNamed: R(pc + 1); e.rd(acc); break;
    case Op::GetKeyed: R(pc + 1); e.rd(acc); e.wr(acc); break;
    case Op::SetKeyed: R(pc + 1); R(pc + 2); e.rd(acc); break;
    case Op::StaEnvSlotAt: case Op::StaEnvSlot: case Op::StaEnvSlotInit: case Op::StaEnv: case Op::StaEnvInit: case Op::StaLookup:
    case Op::CheckObjectCoercible:
        e.rd(acc); break;
    case Op::LdaEnv: case Op::CreateClosure: case Op::CreateObject: case Op::CreateArray: case Op::LdaTdz:
        e.wr(acc); break;
    case Op::LdaEnvStar: e.wr(acc); W(pc + 3); break;
    case Op::LdaTdzStar: e.wr(acc); W(pc + 1); break;
    case Op::TypeOf: case Op::ToPropertyKey: e.rd(acc); e.wr(acc); break;
    case Op::EnterLoopEnv: case Op::AdvanceLoopEnv: case Op::ExitLoopEnv: break;
    case Op::DefineOwn: R(pc + 1); e.rd(acc); break;
    case Op::DefineElement: R(pc + 1); R(pc + 2); e.rd(acc); break;
    case Op::Call: {
        R(pc + 1);
        const uint32_t start = code[pc + 2], argc = code[pc + 3];
        for (uint32_t i = 0; i < argc; i++) e.rd(start + i);
        e.wr(acc);
        break;
    }
    case Op::Construct: {
        R(pc + 1);
        const uint32_t start = code[pc + 2], argc = code[pc + 3];
        for (uint32_t i = 0; i < argc; i++) e.rd(start + i);
        e.wr(acc);
        break;
    }
    case Op::CallResolved: {
        R(pc + 1); R(pc + 2);
        const uint32_t start = code[pc + 3], argc = code[pc + 4];
        for (uint32_t i = 0; i < argc; i++) e.rd(start + i);
        e.wr(acc);
        break;
    }
    default:
        e.read_all = true;
        break;
    }
    (void)nregs;
    return e;
}

inline void Builder::compute_liveness() {
    const BytecodeChunk& c = *F().chunk;
    const uint8_t* code = c.code.data();
    const size_t n = F().insns.size();
    const uint32_t fvars = F().nregs + 1;
    const uint32_t words = (fvars + 63) / 64;
    F().live_words = words;
    F().live_before.assign(n * words, 0);
    std::vector<Effects> eff(n);
    for (size_t i = 0; i < n; i++) eff[i] = op_effects(static_cast<Op>(F().insns[i].op), code, F().insns[i].pc, F().nregs, F().nregs);
    // Successors of an instruction: fall-through unless it ends a path, plus a branch target.
    auto succs = [&](size_t i, int32_t out[2]) {
        const Insn& in = F().insns[i];
        const Op op = static_cast<Op>(in.op);
        int k = 0;
        const int pos = branch_offset_pos(op);
        if (pos >= 0) {
            const int64_t t = static_cast<int64_t>(in.pc) + in.len + i16(in.pc + pos);
            if (t >= 0 && t < static_cast<int64_t>(c.code.size()) && F().insn_at[t] >= 0) out[k++] = F().insn_at[t];
        }
        if (op != Op::Jump && op != Op::Return && op != Op::Throw && i + 1 < n) out[k++] = static_cast<int32_t>(i + 1);
        return k;
    };
    std::vector<uint64_t> tmp(words);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t ri = n; ri-- > 0;) {
            std::fill(tmp.begin(), tmp.end(), 0);
            int32_t sc[2];
            const int ns = succs(ri, sc);
            for (int k = 0; k < ns; k++)
                for (uint32_t w = 0; w < words; w++) tmp[w] |= F().live_before[static_cast<size_t>(sc[k]) * words + w];
            const Effects& e = eff[ri];
            if (!e.read_all) {
                for (uint8_t k = 0; k < e.nw; k++) tmp[e.w[k] >> 6] &= ~(1ull << (e.w[k] & 63));
                for (uint8_t k = 0; k < e.nr; k++) if (e.r[k] < fvars) tmp[e.r[k] >> 6] |= 1ull << (e.r[k] & 63);
            } else {
                for (uint32_t v = 0; v < fvars; v++) tmp[v >> 6] |= 1ull << (v & 63);
            }
            uint64_t* dst = &F().live_before[ri * words];
            for (uint32_t w = 0; w < words; w++)
                if (dst[w] != tmp[w]) { dst[w] = tmp[w]; changed = true; }
        }
    }
}

// ===========================================================================
// SSA construction
// ===========================================================================

inline int32_t Builder::new_phi(uint32_t v, int32_t blk) {
    if (frozen_) fail("phi created after construction");
    const int32_t id = add_node(blk, K::Phi, Rep::Tagged, -1, -1, v);
    auto& nodes = g_.b[blk].nodes;
    // Phis stay at the front, in creation order.
    size_t at = 0;
    while (at < nodes.size() && g_.n[nodes[at]].op == K::Phi) at++;
    nodes.insert(nodes.begin() + at, id);
    return id;
}

inline int32_t Builder::read_var(uint32_t v, int32_t blk) {
    const int32_t d = g_.b[blk].def[v];
    if (d != kUnset) return resolve(d);
    return resolve(read_var_recursive(v, blk));
}

inline int32_t Builder::read_var_recursive(uint32_t v, int32_t blk) {
    int32_t val;
    if (!g_.b[blk].sealed) {
        // Operands are filled in when the block seals (every predecessor has been built).
        val = new_phi(v, blk);
        g_.b[blk].incomplete.push_back({v, val});
    } else if (g_.b[blk].preds.empty()) {
        val = kMem;
    } else if (g_.b[blk].preds.size() == 1) {
        val = read_var(v, g_.b[blk].preds[0]);
    } else {
        val = new_phi(v, blk);
        write_var(v, blk, val);   // breaks cycles
        val = add_phi_operands(v, val);
    }
    write_var(v, blk, val);
    return val;
}

inline int32_t Builder::add_phi_operands(uint32_t v, int32_t phi) {
    const int32_t blk = g_.n[phi].blk;
    const size_t np = g_.b[blk].preds.size();
    for (size_t i = 0; i < np; i++) {
        const int32_t p = g_.b[blk].preds[i];
        const int32_t d = norm(v, read_var(v, p));
        g_.n[phi].ins.push_back(d);
    }
    return try_remove_trivial(phi);
}

inline int32_t Builder::try_remove_trivial(int32_t phi) {
    int32_t same = kUnset;
    for (int32_t op : g_.n[phi].ins) {
        op = resolve(op);
        if (op == same || op == phi) continue;
        if (same != kUnset) return phi;
        same = op;
    }
    if (same == kUnset) same = kMem;   // only itself: reads as memory
    if (same == kMem) {
        // Every path into the block has the variable in the register bank, so the phi is a load of
        // it. Uses that already name the phi resolve to this load; it goes right after the phis, so
        // it runs before anything in the block that could change the bank.
        const uint32_t v = g_.n[phi].aux;
        const int32_t blk = g_.n[phi].blk;
        same = add_node(blk, v == g_.acc_var ? K::LoadAcc : K::LoadReg, Rep::Tagged, -1, -1, v, memver_[v]);
        auto& nodes = g_.b[blk].nodes;
        size_t at = 0;
        while (at < nodes.size() && g_.n[nodes[at]].op == K::Phi) at++;
        nodes.insert(nodes.begin() + at, same);
    }
    g_.repl[phi] = same;
    g_.n[phi].dead = true;
    return same;
}

inline void Builder::seal(int32_t blk) {
    std::vector<std::pair<uint32_t, int32_t>> pending = std::move(g_.b[blk].incomplete);
    g_.b[blk].incomplete.clear();
    g_.b[blk].sealed = true;
    for (auto& [v, phi] : pending) {
        if (g_.n[phi].dead) continue;
        add_phi_operands(v, phi);
    }
}

inline int32_t Builder::snapshot_at(int32_t blk, int32_t fi, uint32_t pc, int32_t edge_var) {
    // A callee whose every guard can leave through the call itself (it has done nothing observable yet).
    if (g_.funcs[fi].leaf && g_.funcs[fi].entry_state >= 0) return g_.funcs[fi].entry_state;
    State s;
    s.func = fi;
    s.pc = pc;
    s.edge_var = edge_var;
    std::vector<uint32_t> vars;
    state_vars(fi, pc, vars);        // the variables the interpreter may read: the rest need no write-back
    for (uint32_t v : vars) {
        const int32_t d = resolve(read_var(v, blk));
        if (d == kMem || is_own_load(v, d)) continue;
        s.defs.push_back({v, d});
    }
    if (!g_.states.empty() && g_.states.back().func == s.func && g_.states.back().pc == s.pc && g_.states.back().edge_var == s.edge_var && g_.states.back().defs == s.defs)
        return static_cast<int32_t>(g_.states.size() - 1);
    g_.states.push_back(std::move(s));
    return static_cast<int32_t>(g_.states.size() - 1);
}

// ===========================================================================
// Typing helpers
// ===========================================================================

inline int32_t Builder::const_of(const Value& v) {
    if (v.is_number()) {
        const double d = v.as_number();
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        return emit(K::ConstF64, Rep::F64, -1, -1, 0, bits);
    }
    if (v.is_boolean()) return emit(K::ConstBool, Rep::Bool, -1, -1, 0, v.as_boolean() ? 1 : 0);
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    return emit(K::ConstTagged, Rep::Tagged, -1, -1, 0, bits);
}

inline int32_t Builder::num(int32_t x) {
    x = resolve(x);
    const Node& nd = g_.n[x];
    if (nd.rep == Rep::F64) return x;
    if (nd.rep == Rep::Bool || nd.rep == Rep::I32) return -1;
    if (nd.op == K::ConstTagged) return -1;   // undefined, null, a string...: not a number
    return guarded(K::ToNum, Rep::F64, x);
}

inline int32_t Builder::tagged(int32_t x) {
    x = resolve(x);
    if (g_.n[x].rep == Rep::Tagged && g_.n[x].op != K::Phi) return x;
    return emit(K::ToTagged, Rep::Tagged, x);
}

inline int32_t Builder::boolean(int32_t x) {
    x = resolve(x);
    if (g_.n[x].rep == Rep::Bool) return x;
    return emit(K::ToBool, Rep::Bool, x);
}

// ===========================================================================
// Builder
// ===========================================================================

inline void Builder::flush_all(int32_t live_pc) {
    for (uint32_t v = 0; v <= g_.acc_var; v++) {         // only the root has a register bank to write back to
        const int32_t d = resolve(read_var(v, cb_));
        if (d == kMem) continue;
        if (!is_own_load(v, d) && (live_pc < 0 || live_at(static_cast<uint32_t>(live_pc), v))) {
            const int32_t t = tagged(d);
            if (v == g_.acc_var) emit(K::StoreAcc, Rep::None, t);
            else emit(K::StoreReg, Rep::None, t, -1, v);
        }
        write_var(v, cb_, kMem);
    }
    state_valid_ = false;
}

inline void Builder::step(const Insn& in, bool& ended) {
    const Op op = static_cast<Op>(in.op);
    const int pos = branch_offset_pos(op);
    // An instruction whose registers are all known (op_effects) is a light step: only what it reads is written back,
    // and the other values stay in the unit's registers across it; what it writes is read from the bank afterwards.
    Effects eff;
    bool light = false;
    if (config().light && cf_ == 0 && !F().no_light && pos < 0 && op != Op::Throw && op != Op::Return) {
        eff = op_effects(op, F().chunk->code.data(), in.pc, F().nregs, F().nregs);
        light = !eff.read_all;
    }
    if (light) {
        for (uint8_t k = 0; k < eff.nr; k++) {
            const uint32_t v = eff.r[k];
            const int32_t d = resolve(read_var(v, cb_));
            if (d == kMem || is_own_load(v, d)) continue;
            const int32_t t = tagged(d);
            if (v == g_.acc_var) emit(K::StoreAcc, Rep::None, t);
            else emit(K::StoreReg, Rep::None, t, -1, v);
        }
    } else {
        flush_all(in.pc);       // what the interpreter may still read at this instruction
    }
    const int64_t target_pc = pos >= 0 ? static_cast<int64_t>(in.pc) + in.len + i16(in.pc + pos) : -1;
    const int32_t s = emit(K::Step, Rep::I32, -1, -1, in.pc, pos >= 0 ? static_cast<uint64_t>(target_pc) : kNoPc);
    g_.n[s].light = light;
    if (light) {
        for (uint8_t k = 0; k < eff.nw; k++) { write_var(eff.w[k], cb_, kMem); memver_[eff.w[k]]++; }
        state_valid_ = false;
    }
    const uint32_t next_pc = in.pc + in.len;
    Block& B = g_.b[cb_];
    if (op == Op::Throw) {
        // Nothing falls out of this: the step always leaves.
        emit(K::StepExit, Rep::None, s, -1, 0, kNoPc);
        B.term.kind = T::Exit;
        ended = true;
        return;
    }
    if (pos >= 0) {
        B.term.kind = T::StepBranch;
        B.term.cond = s;
        B.term.tpc = static_cast<uint32_t>(target_pc);
        B.term.fpc = next_pc;
        B.term.t = B.succs[0];
        B.term.f = B.succs[1];
        ended = true;
        return;
    }
    emit(K::StepExit, Rep::None, s, -1, 0, next_pc);
}

// ===========================================================================
// Translation of the opcodes the tier handles itself
// ===========================================================================

inline bool Builder::translate(const Insn& in, bool& ended) {
    const uint8_t* code = F().chunk->code.data();
    const Op op = static_cast<Op>(in.op);
    const uint32_t pc = in.pc;
    if (cf_ == 0 && avoided(pc)) return false;
    auto reg = [&](uint32_t at) -> uint32_t { return F().var_base + code[at]; };
    auto ok_reg = [&](uint32_t r) { return r < F().acc_var; };
    // Looks at a node and lets go of it before anything is emitted (emitting can move g_.n).
    auto numeric_value = [&](int32_t x) { const Node& nd = g_.n[resolve(x)]; return nd.rep != Rep::Bool && nd.rep != Rep::I32 && nd.op != K::ConstTagged; };
    auto object_value = [&](int32_t x) { const Node& nd = g_.n[resolve(x)]; return nd.rep == Rep::Tagged && nd.op != K::ConstTagged; };
    // Both operands numbers, or the instruction is not ours.
    auto number_operands = [&](int32_t l, int32_t r, int32_t& a, int32_t& b) {
        auto numeric = [&](int32_t x) { const Node& nd = g_.n[resolve(x)]; return nd.rep != Rep::Bool && nd.rep != Rep::I32 && nd.op != K::ConstTagged; };
        if (!numeric(l) || !numeric(r)) return false;
        a = num(l);
        b = num(r);
        return true;
    };

    switch (op) {
    case Op::LdaConst: {
        const uint32_t k = u16(pc + 1);
        if (k >= F().chunk->constants.size()) return false;
        set(acc(), const_of(F().chunk->constants[k]));
        return true;
    }
    case Op::LdaZero: set(acc(), const_of(Value(0.0))); return true;
    case Op::LdaSmi: set(acc(), const_of(Value(static_cast<double>(static_cast<int8_t>(code[pc + 1]))))); return true;
    case Op::LdaUndefined: set(acc(), const_of(Value())); return true;
    case Op::LdaNull: set(acc(), const_of(Value::null())); return true;
    case Op::LdaTrue: set(acc(), const_of(Value(true))); return true;
    case Op::LdaFalse: set(acc(), const_of(Value(false))); return true;
    case Op::Ldar: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        set(acc(), value(r));
        return true;
    }
    case Op::Star: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        set(r, value(acc()));
        return true;
    }
    case Op::Mov: {
        const uint32_t src = reg(pc + 1), dst = reg(pc + 2);
        if (!ok_reg(src) || !ok_reg(dst)) return false;
        set(dst, value(src));
        return true;
    }
    case Op::LdarStar: {
        const uint32_t src = reg(pc + 1), dst = reg(pc + 2);
        if (!ok_reg(src) || !ok_reg(dst)) return false;
        const int32_t v = value(src);
        set(acc(), v);
        set(dst, v);
        return true;
    }
    case Op::LdaSmiStar: {
        const uint32_t dst = reg(pc + 2);
        if (!ok_reg(dst)) return false;
        const int32_t v = const_of(Value(static_cast<double>(static_cast<int8_t>(code[pc + 1]))));
        set(acc(), v);
        set(dst, v);
        return true;
    }
    case Op::LdaZeroStar: {
        const uint32_t dst = reg(pc + 1);
        if (!ok_reg(dst)) return false;
        const int32_t v = const_of(Value(0.0));
        set(acc(), v);
        set(dst, v);
        return true;
    }
    case Op::LdaConstStar: {
        const uint32_t k = u16(pc + 1), dst = reg(pc + 3);
        if (k >= F().chunk->constants.size() || !ok_reg(dst)) return false;
        const int32_t v = const_of(F().chunk->constants[k]);
        set(acc(), v);
        set(dst, v);
        return true;
    }
    case Op::LdarChecked: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        const int32_t v = guarded(K::NotTdz, Rep::Tagged, value(r));
        set(acc(), v);
        return true;
    }
    case Op::StarChecked: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        const int32_t a = value(acc());
        guarded(K::NotTdz, Rep::Tagged, value(r));
        set(r, a);
        return true;
    }

    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        int32_t a, b;
        if (!number_operands(value(r), value(acc()), a, b)) return false;
        const K k = op == Op::Add ? K::AddF : op == Op::Sub ? K::SubF : op == Op::Mul ? K::MulF : op == Op::Mod ? K::ModF : K::DivF;
        set(acc(), emit(k, Rep::F64, a, b));
        return true;
    }
    case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Sar: case Op::Shr: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        int32_t a, b;
        if (!number_operands(value(r), value(acc()), a, b)) return false;
        const int32_t ia = guarded(K::ToI32, Rep::I32, a);
        const int32_t ib = guarded(K::ToI32, Rep::I32, b);
        K k = K::AndI;
        switch (op) {
        case Op::BitOr: k = K::OrI; break;
        case Op::BitXor: k = K::XorI; break;
        case Op::Shl: k = K::ShlI; break;
        case Op::Sar: k = K::SarI; break;
        case Op::Shr: k = K::ShrI; break;
        default: break;
        }
        const int32_t res = emit(k, Rep::I32, ia, ib);
        set(acc(), emit(op == Op::Shr ? K::U32ToF64 : K::I32ToF64, Rep::F64, res));
        return true;
    }
    case Op::TestLt: case Op::TestGt: case Op::TestLe: case Op::TestGe: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        int32_t a, b;
        if (!number_operands(value(r), value(acc()), a, b)) return false;
        const K k = op == Op::TestLt ? K::LtF : op == Op::TestGt ? K::GtF : op == Op::TestLe ? K::LeF : K::GeF;
        set(acc(), emit(k, Rep::Bool, a, b));
        return true;
    }
    case Op::TestEq: case Op::TestNe: {
        // Two numbers compare the same loose as strict; anything else is the interpreter's.
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        const int32_t l = value(r), rr = value(acc());
        auto nullish_const = [&](int32_t x) {
            const Node& nd = g_.n[resolve(x)];
            if (nd.op != K::ConstTagged) return false;
            Value v;
            std::memcpy(&v, &nd.imm, 8);
            return v.is_undefined() || v.is_null();
        };
        // `x == null` and `x == undefined` hold for exactly those two.
        if (nullish_const(rr) || nullish_const(l)) {
            const int32_t other = nullish_const(rr) ? l : rr;
            const int32_t is = emit(K::IsNullish, Rep::Bool, tagged(other));
            set(acc(), op == Op::TestEq ? is : emit(K::NotBool, Rep::Bool, is));
            return true;
        }
        int32_t a, b;
        if (!number_operands(l, rr, a, b)) return false;
        set(acc(), emit(op == Op::TestEq ? K::EqF : K::NeF, Rep::Bool, a, b));
        return true;
    }
    case Op::TestStrictEq: case Op::TestStrictNe: {
        const uint32_t r = reg(pc + 1);
        if (!ok_reg(r)) return false;
        const bool eq = op == Op::TestStrictEq;
        const int32_t l = value(r), rr = value(acc());
        auto is_id_const = [&](int32_t x) {   // undefined, null or a boolean: strict equality is bit identity
            const Node& nd = g_.n[resolve(x)];
            if (nd.op == K::ConstBool) return true;
            if (nd.op != K::ConstTagged) return false;
            Value v;
            std::memcpy(&v, &nd.imm, 8);
            return v.is_undefined() || v.is_null();
        };
        int32_t a, b;
        if (number_operands(l, rr, a, b)) {
            set(acc(), emit(eq ? K::EqF : K::NeF, Rep::Bool, a, b));
            return true;
        }
        if (is_id_const(l) || is_id_const(rr)) {
            set(acc(), emit(eq ? K::EqTag : K::NeTag, Rep::Bool, tagged(l), tagged(rr)));
            return true;
        }
        return false;
    }
    case Op::Neg: {
        if (!numeric_value(value(acc()))) return false;
        set(acc(), emit(K::NegF, Rep::F64, num(value(acc()))));
        return true;
    }
    case Op::Inc: case Op::Dec: {
        if (!numeric_value(value(acc()))) return false;
        const int32_t a = num(value(acc()));
        const int32_t one = const_of(Value(1.0));
        set(acc(), emit(op == Op::Inc ? K::AddF : K::SubF, Rep::F64, a, one));
        return true;
    }
    case Op::BitNot: {
        if (!numeric_value(value(acc()))) return false;
        const int32_t ia = guarded(K::ToI32, Rep::I32, num(value(acc())));
        set(acc(), emit(K::I32ToF64, Rep::F64, emit(K::NotI, Rep::I32, ia)));
        return true;
    }
    case Op::LogicalNot: {
        set(acc(), emit(K::NotBool, Rep::Bool, boolean(value(acc()))));
        return true;
    }
    case Op::ToNumber: case Op::ToNumeric: {
        if (!numeric_value(value(acc()))) return false;
        set(acc(), num(value(acc())));
        return true;
    }

    case Op::LdaThis: case Op::LdaThisStar: {
        if (cf_ == 0 && !g_cfg.this_needs_super_inline) return false;
        const bool fused = op == Op::LdaThisStar;
        const uint32_t dst = fused ? reg(pc + 1) : 0;
        if (fused && !ok_reg(dst)) return false;
        const int32_t v = cf_ != 0 ? value(static_cast<uint32_t>(F().this_var)) : guarded(K::LoadThis, Rep::Tagged, -1);
        set(acc(), v);
        if (fused) set(dst, v);
        return true;
    }
    case Op::LdaEnvSlot: case Op::LdaEnvSlotStar: {
        if (cf_ != 0) return false;      // the environments belong to the root's frame
        const bool fused = op == Op::LdaEnvSlotStar;
        const uint32_t slot = reg(pc + 1), nidx = u16(pc + 2);
        const uint32_t dst = fused ? reg(pc + 4) : 0;
        if ((fused && !ok_reg(dst)) || nidx >= F().chunk->names.size() || slot >= JitEnvLayout::kMaxSlots) return false;
        const int32_t v = guarded(K::LoadEnv, Rep::Tagged, -1, -1, slot, reinterpret_cast<uint64_t>(F().chunk->names[nidx]));
        set(acc(), v);
        if (fused) set(dst, v);
        return true;
    }
    case Op::LdaEnvSlotAt: {
        if (cf_ != 0) return false;
        const uint32_t hops = reg(pc + 1), slot = reg(pc + 2), nidx = u16(pc + 3);
        if (nidx >= F().chunk->names.size() || slot >= JitEnvLayout::kMaxSlots || hops > 16) return false;
        set(acc(), guarded(K::LoadEnv, Rep::Tagged, -1, -1, slot | (hops << 8), reinterpret_cast<uint64_t>(F().chunk->names[nidx])));
        return true;
    }
    case Op::StaEnvSlotAt: {
        if (cf_ != 0) return false;
        const uint32_t hops = reg(pc + 1), slot = reg(pc + 2), nidx = u16(pc + 3);
        if (nidx >= F().chunk->names.size() || slot >= JitEnvLayout::kMaxSlots || hops > 16) return false;
        const int32_t val = tagged(value(acc()));
        guarded(K::StoreEnv, Rep::None, val, -1, slot | (hops << 8), reinterpret_cast<uint64_t>(F().chunk->names[nidx]));
        return true;
    }
    case Op::StaEnvSlot: {
        if (cf_ != 0) return false;
        const uint32_t slot = reg(pc + 1), nidx = u16(pc + 2);
        if (nidx >= F().chunk->names.size() || slot >= JitEnvLayout::kMaxSlots) return false;
        const int32_t val = tagged(value(acc()));
        guarded(K::StoreEnv, Rep::None, val, -1, slot, reinterpret_cast<uint64_t>(F().chunk->names[nidx]));
        return true;
    }

    case Op::LdaLookup: case Op::LdaLookupStar: {
        const bool fused = op == Op::LdaLookupStar;
        const uint32_t nidx = u16(pc + 1), dst = fused ? reg(pc + 3) : 0;
        if ((fused && !ok_reg(dst)) || nidx >= F().chunk->names.size() || nidx >= F().chunk->lookup_cache.size()) return false;
        const BytecodeChunk::LookupCacheEntry* base = cf_ == 0 ? (g_.frame ? g_.frame->lookup_cache_data : nullptr) : F().lookup;
        if (!base) return false;
        const BytecodeChunk::LookupCacheEntry& e = base[nidx];
        if (e.shadow_epoch != Environment::binding_shadow_epoch()) return false;
        uint32_t form;
        if (e.dict_desc) form = 2; else if (e.obj_shape) form = 1; else if (e.slot) form = 0; else return false;
        const bool root = cf_ == 0;
        const int32_t v = guarded(K::LoadLookup, Rep::Tagged, -1, -1, form | (root ? 0x100u : 0u),
                                  root ? static_cast<uint64_t>(nidx) * JitLookupLayout::kSize : reinterpret_cast<uint64_t>(&e));
        set(acc(), v);
        if (fused) set(dst, v);
        return true;
    }
    case Op::GetNamed: case Op::GetNamedStar: {
        const bool fused = op == Op::GetNamedStar;
        const uint32_t rcv = reg(pc + 1), fbi = u16(pc + 4);
        const uint32_t dst = fused ? reg(pc + 6) : 0;
        if (!ok_reg(rcv) || (fused && !ok_reg(dst)) || fbi >= F().chunk->feedback.size()) return false;
        if (F().ctor && resolve(value(rcv)) == resolve(value(static_cast<uint32_t>(F().this_var)))) return false;   // mid-construction shapes
        const FeedbackBody& fb = F().chunk->feedback[fbi].read();
        const bool own = fb.count >= 1 && !fb.mega && !fb.entries[0].is_accessor;
        const bool array_len = !own && fb.array_length && fb.count == 0;
        // A property in the receiver's own descriptor map (every builtin namespace function): the receiver is known by
        // identity and the value by the descriptor epoch.
        const bool own_desc = !own && !array_len && fb.count == 0 && fb.own_desc_receiver && fb.own_desc_epoch == Object::descriptor_epoch();
        // A method (or other data property) found on a prototype: the receiver's shape and prototype and the global
        // epochs are what the interpreter's entry is valid under.
        const FeedbackBody::ProtoEntry* inh = nullptr;
        if (!own && !array_len && fb.count == 0 && !fb.mega && !fb.proto_mega && fb.proto_count == 1) {
            const FeedbackBody::ProtoEntry& pe = fb.proto_entries[0];
            if (!pe.is_getter && !pe.absent && !pe.is_setter && pe.receiver_word && pe.prototype && pe.proto_epoch == Object::proto_epoch() &&
                (pe.from_descriptor ? pe.desc_epoch == Object::descriptor_epoch() : pe.holder != nullptr))
                inh = &pe;
        }
        if (config().log) std::fprintf(stderr, "[opt] GetNamed pc=%u fb: count=%u mega=%d accessor=%d array_length=%d proto_count=%u\n", pc, fb.count, fb.mega, fb.count ? fb.entries[0].is_accessor : -1, fb.array_length, fb.proto_count);
        if (own_desc) {
            if (!object_value(value(rcv))) return false;
            Object* r = fb.own_desc_receiver;
            const Value rv = r->get_type() == Object::ObjectType::Function ? Value(static_cast<Function*>(r)) : Value(r);
            uint64_t rbits;
            std::memcpy(&rbits, &rv, 8);
            guarded(K::CheckBits, Rep::None, tagged(value(rcv)), -1, 0, rbits);
            epoch_guard(1, fb.own_desc_epoch);
            const int32_t v = const_of(fb.own_desc_value);
            set(acc(), v);
            if (fused) set(dst, v);
            return true;
        }
        if (!own && !array_len && !inh) return false;
        if (!object_value(value(rcv))) return false;
        const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
        int32_t v;
        if (own && fb.count >= 2 && fb.count <= 4) {
            Graph::Poly p;
            bool plain = true;
            for (uint8_t k = 0; k < fb.count; k++) {
                if (fb.entries[k].is_accessor || (fb.entries[k].word & FeedbackBody::Entry::kSlow)) plain = false;
                p.word[k] = fb.entries[k].word;
                p.slot[k] = fb.entries[k].slot_index;
            }
            p.n = fb.count;
            if (plain) {
                g_.polys.push_back(p);
                v = guarded(K::LoadPoly, Rep::Tagged, obj, -1, static_cast<uint32_t>(g_.polys.size() - 1));
                set(acc(), v);
                if (fused) set(dst, v);
                return true;
            }
        }
        if (inh) {
            guarded(K::CheckShape, Rep::None, obj, -1, 0, inh->receiver_word);
            guarded(K::CheckProto, Rep::None, obj, -1, 0, reinterpret_cast<uint64_t>(inh->prototype));
            epoch_guard(0, inh->proto_epoch);
            if (inh->from_descriptor) {
                epoch_guard(1, inh->desc_epoch);
                v = const_of(inh->cached_value);
            } else {
                v = emit(K::LoadHolder, Rep::Tagged, -1, -1, inh->slot_index, reinterpret_cast<uint64_t>(inh->holder));
            }
        } else if (own) {
            guarded(K::CheckShape, Rep::None, obj, -1, 0, fb.entries[0].word);
            v = emit(K::LoadSlot, Rep::Tagged, obj, -1, fb.entries[0].slot_index);
        } else {
            guarded(K::CheckDense, Rep::None, obj);
            v = emit(K::ArrayLen, Rep::F64, obj);
        }
        set(acc(), v);
        if (fused) set(dst, v);
        return true;
    }
    case Op::SetNamed: {
        const uint32_t rcv = reg(pc + 1), fbi = u16(pc + 4);
        if (!ok_reg(rcv) || fbi >= F().chunk->feedback.size()) return false;
        if (F().ctor) {
            // `this.name = value` in an inlined constructor: the object already has its final shape.
            uint32_t slot = 0;
            bool found = false;
            for (auto& [spc, sslot] : g_.ctors[F().plan].stores) if (spc == pc) { slot = sslot; found = true; }
            if (!found || resolve(value(rcv)) != resolve(value(static_cast<uint32_t>(F().this_var)))) return false;
            const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
            const int32_t val = tagged(value(acc()));
            emit(K::StoreSlot, Rep::None, obj, val, slot);
            return true;
        }
        const FeedbackBody& fb = F().chunk->feedback[fbi].read();
        if (fb.count < 1 || fb.mega || fb.entries[0].is_accessor) return false;
        if (!object_value(value(rcv))) return false;
        const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
        guarded(K::CheckShape, Rep::None, obj, -1, 0, fb.entries[0].word);
        const int32_t val = tagged(value(acc()));
        emit(K::StoreSlot, Rep::None, obj, val, fb.entries[0].slot_index);
        return true;
    }
    case Op::GetKeyed: {
        const uint32_t rcv = reg(pc + 1);
        if (!ok_reg(rcv)) return false;
        if (!dense_array_receiver(rcv)) return false;
        if (!object_value(value(rcv)) || !numeric_value(value(acc()))) return false;
        const int32_t idx = guarded(K::ToIndex, Rep::I32, num(value(acc())));
        const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
        guarded(K::CheckDense, Rep::None, obj);
        set(acc(), guarded(K::LoadElem, Rep::Tagged, obj, idx));
        return true;
    }
    case Op::SetKeyed: {
        if (F().ctor) return false;
        const uint32_t rcv = reg(pc + 1), kreg = reg(pc + 2);
        if (!ok_reg(rcv) || !ok_reg(kreg)) return false;
        if (!dense_array_receiver(rcv)) return false;
        if (!object_value(value(rcv)) || !numeric_value(value(kreg))) return false;
        const int32_t idx = guarded(K::ToIndex, Rep::I32, num(value(kreg)));
        const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
        guarded(K::CheckDense, Rep::None, obj);
        const int32_t val = tagged(value(acc()));
        guarded(K::StoreElem, Rep::None, obj, idx, 0, 0, val);
        return true;
    }

    case Op::Jump: {
        Block& B = g_.b[cb_];
        B.term.kind = T::Jump;
        B.term.t = B.succs[0];
        ended = true;
        return true;
    }
    case Op::LdaTdz: case Op::LdaTdzStar: {
        const bool fused = op == Op::LdaTdzStar;
        const uint32_t dst = fused ? reg(pc + 1) : 0;
        if (fused && !ok_reg(dst)) return false;
        uint64_t tdz = kTdzBits;
        const int32_t v = emit(K::ConstTagged, Rep::Tagged, -1, -1, 0, tdz);
        set(acc(), v);
        if (fused) set(dst, v);
        return true;
    }
    case Op::CheckObjectCoercible: {
        const int32_t v = tagged(value(acc()));
        set(acc(), guarded(K::CheckNotNullish, Rep::Tagged, v));
        return true;
    }
    case Op::JumpIfNullish: case Op::JumpIfNotNullish: case Op::JumpIfNotUndefined: {
        const int32_t v = tagged(value(acc()));
        int32_t c;
        if (op == Op::JumpIfNotUndefined) {
            const int32_t undef = const_of(Value());
            c = emit(K::NotBool, Rep::Bool, emit(K::EqTag, Rep::Bool, v, undef));
        } else {
            c = emit(K::IsNullish, Rep::Bool, v);
            if (op == Op::JumpIfNotNullish) c = emit(K::NotBool, Rep::Bool, c);
        }
        Block& B = g_.b[cb_];
        B.term.kind = T::Branch;
        B.term.cond = c;
        B.term.t = B.succs[0];       // the branch target; the fall-through is succs[1]
        B.term.f = B.succs[1];
        ended = true;
        return true;
    }
    case Op::CreateObjectT: {
        const uint32_t ti = u16(pc + 1);
        const auto* ic = F().chunk->ic_feedback.get();
        if (!ic || ti >= ic->literal_templates.size() || !ic->literal_templates[ti].shape) return false;
        set(acc(), guarded(K::NewLiteral, Rep::Tagged, -1, -1, 0, reinterpret_cast<uint64_t>(&ic->literal_templates[ti])));
        return true;
    }
    case Op::DefineOwn: {
        // A property of a literal made from a template: the object has its final shape, the value goes in its slot.
        const uint32_t rcv = reg(pc + 1), fbi = u16(pc + 4);
        if (!ok_reg(rcv) || fbi >= F().chunk->feedback.size()) return false;
        const FeedbackBody* fb = F().chunk->feedback[fbi].peek();
        if (!fb || !fb->lit_shape || !object_value(value(rcv))) return false;
        const int32_t obj = guarded(K::CheckObj, Rep::Ptr, tagged(value(rcv)));
        guarded(K::CheckShape, Rep::None, obj, -1, 0, reinterpret_cast<uint64_t>(fb->lit_shape) | JitObjectLayout::kOrdinaryType);
        emit(K::StoreSlot, Rep::None, obj, tagged(value(acc())), fb->lit_slot);
        return true;
    }
    case Op::JumpIfTrue: case Op::JumpIfFalse: {
        Block& B = g_.b[cb_];
        const int32_t c = boolean(value(acc()));
        B.term.kind = T::Branch;
        B.term.cond = c;
        // succs[0] is the branch target, succs[1] the fall-through.
        B.term.t = op == Op::JumpIfTrue ? B.succs[0] : B.succs[1];
        B.term.f = op == Op::JumpIfTrue ? B.succs[1] : B.succs[0];
        ended = true;
        return true;
    }
    case Op::Return: {
        int32_t v = tagged(value(acc()));
        if (cf_ != 0 && F().ctor) {
            // `new` yields the object unless the constructor returns another: only a body that returns nothing is inlined.
            const Node& rn = g_.n[resolve(v)];
            if (rn.op != K::ConstTagged || rn.imm != std::bit_cast<uint64_t>(Value())) return false;
            v = tagged(value(static_cast<uint32_t>(F().this_var)));
        }
        Block& B = g_.b[cb_];
        if (cf_ != 0) {
            // An inlined callee: the result is the caller's accumulator and control goes on after the call.
            if (B.succs.size() != 1) return fail("inlined return without its continuation");
            B.term.kind = T::Jump;
            B.term.t = B.succs[0];
            set(g_.funcs[F().parent].acc_var, v);
            ended = true;
            return true;
        }
        B.term.kind = T::Return;
        B.term.cond = v;
        ended = true;
        return true;
    }
    case Op::Call: case Op::CallResolved: case Op::Construct: {
        const int32_t callee = site_of(cf_, pc);
        if (callee < 0) {
            const int32_t poly = poly_of(cf_, pc);
            if (poly >= 0) {
                if (!poly_dispatch(poly, 0, ended)) {
                    if (!g_.failed_site.first) g_.failed_site = {F().chunk, pc};
                    fail("a polymorphic call could not be entered");
                    g_.b[cb_].term.kind = T::Bail;
                    ended = true;
                }
                return true;
            }
            return op != Op::Construct && native_call(in);
        }
        if (!inline_enter(in, callee, ended)) {
            // The blocks of the callee are already wired in: it is all or nothing, so build again without this site.
            if (!g_.failed_site.first) g_.failed_site = {F().chunk, pc};
            fail("an inlined call could not be entered");
            g_.b[cb_].term.kind = T::Bail;
            ended = true;
        }
        return true;
    }
    default:
        return false;
    }
}

inline bool Builder::translate_block(int32_t blk) {
    cb_ = blk;
    Block& B0 = g_.b[blk];
    cf_ = B0.func;
    if (B0.exit_pc >= 0) {
        flush_all(B0.exit_pc);
        g_.b[blk].term.kind = T::Bail;
        g_.b[blk].term.tpc = static_cast<uint32_t>(B0.exit_pc);
        return true;
    }
    if (B0.poly_kind) {
        auto& ps = F().polys[B0.poly_site];
        begin_op(ps.call.pc);
        bool ended = false;
        bool ok;
        if (B0.poly_kind == 1) {
            ok = poly_dispatch(B0.poly_site, B0.poly_t, ended);
        } else {
            const bool last = static_cast<size_t>(B0.poly_t) + 1 == ps.callees.size();
            ok = inline_define(ps.call, ps.callees[B0.poly_t], ps.fv, last);
            Block& B = g_.b[blk];
            B.term.kind = T::Jump;
            B.term.t = B.succs[0];
        }
        if (!ok) {
            if (!g_.failed_site.first) g_.failed_site = {F().chunk, ps.pc};
            fail("a polymorphic call could not be entered");
            g_.b[blk].term.kind = T::Bail;
        }
        return true;
    }
    if (B0.synthetic || B0.first_insn < 0) {
        // Entry block or landing block: nothing to translate. The terminator is a jump to the only successor.
        B0.term.kind = T::Jump;
        B0.term.t = B0.succs[0];
        return true;
    }
    if (B0.cold) {
        // Never executed: reaching it means the speculation was wrong; leave through the deopt path (which, in a callee,
        // rebuilds the interpreter frames).
        begin_op(F().insns[B0.first_insn].pc);
        guarded(K::Trap, Rep::None, -1);
        g_.b[blk].term.kind = T::Bail;
        g_.b[blk].term.tpc = F().insns[B0.first_insn].pc;
        return true;
    }
    if (B0.loop_header) emit(K::Poll, Rep::None);
    bool ended = false;
    for (int32_t i = B0.first_insn; i <= B0.last_insn && !ended; i++) {
        const Insn in = F().insns[i];
        begin_op(in.pc);
        if (!translate(in, ended)) {
            if (cf_ != 0) {
                // A callee is only worth inlining when all of it is ours: there is no register bank to step it in.
                if (config().log) std::fprintf(stderr, "[opt] callee step %s\n", op_name(static_cast<Op>(in.op)));
                if (!g_.failed_site.first) g_.failed_site = {g_.funcs[F().parent].chunk, F().call_pc};
                return fail("an inlined callee needs a step");
            }
            step(in, ended);
        }
    }
    if (!ended) {
        Block& B = g_.b[blk];
        if (B.succs.size() != 1) return fail("block falls off the end");
        B.term.kind = T::Jump;
        B.term.t = B.succs[0];
    }
    return true;
}

// ===========================================================================
// build()
// ===========================================================================

inline std::vector<int32_t> Builder::compute_rpo() {
    std::vector<int32_t> post;
    std::vector<uint8_t> seen(g_.b.size(), 0);
    std::vector<std::pair<int32_t, size_t>> stack;
    for (int32_t e : g_.entries) {
        if (seen[e]) continue;
        seen[e] = 1;
        stack.push_back({e, 0});
        while (!stack.empty()) {
            auto& [blk, idx] = stack.back();
            if (idx < g_.b[blk].succs.size()) {
                const int32_t s = g_.b[blk].succs[idx++];
                if (!seen[s]) { seen[s] = 1; stack.push_back({s, 0}); }
            } else {
                post.push_back(blk);
                stack.pop_back();
            }
        }
    }
    std::reverse(post.begin(), post.end());
    return post;
}

// ---- inlining ----

// What the feedback says the call at `in` reaches (up to CallSite::kMaxTargets functions), when the tier can take every
// one of those bodies in: plain bytecode (no try regions, no loops), small, in this realm, that the interpreter could run
// stackless (so a deoptimization can rebuild its frame) or that is a leaf (so a deoptimization can leave through the call).
// One target is inlined behind an identity check; several are told apart by a chain of checks.
inline bool Builder::inline_targets(int32_t fi, const Insn& in, std::vector<Function*>& out) {
    auto no = [&](int why) {
        if (config().log) std::fprintf(stderr, "[opt] call at pc %u of chunk %p not inlinable: %d (entry state %u, verdict %u, calls %u)\n", in.pc, static_cast<const void*>(g_.funcs[fi].chunk), why, g_.funcs[fi].chunk->opt_entry, g_.funcs[fi].chunk->inline_state, g_.funcs[fi].chunk->opt_calls);
        if (config().log && why == 12) std::fprintf(stderr, "[opt]   sites recorded: %zu\n", g_.funcs[fi].chunk->ic_feedback ? g_.funcs[fi].chunk->ic_feedback->call_sites.size() : 0);
        return false;
    };
    const Func& P = g_.funcs[fi];
    uint32_t depth = 0;
    for (int32_t k = fi; g_.funcs[k].parent >= 0; k = g_.funcs[k].parent) depth++;
    if (depth >= config().inline_depth || in.pc + in.len >= P.chunk->code.size()) return no(1);
    const Site key{P.chunk, in.pc};
    // A call whose guards kept failing (the Avoid table holds its pc) is left as it is: a step.
    if ((no_inline_ && no_inline_->count(key)) || (avoid_ && (avoid_->no_inline.count(key) || (fi == 0 && avoid_->pcs.count(in.pc))))) return no(2);
    const auto* fb = P.chunk->ic_feedback.get();
    if (!fb) return no(3);
    for (const auto& cs : fb->call_sites) {
        if (cs.pc != in.pc) continue;
        const bool construct = static_cast<Op>(in.op) == Op::Construct;
        if (cs.mega || cs.n == 0 || cs.n > config().poly_max || (construct && cs.n != 1)) return no(4);
        for (uint8_t t = 0; t < cs.n; t++) {
            Function* fn = cs.fns[t];
            if (!fn || fn->get_function_kind() != Function::FunctionKind::Plain || fn->is_arrow()) return no(4);
            const FunctionExecutable* exe = fn->get_executable().get();
            if (!exe || !exe->bytecode_chunk || exe->owner_realm() != g_current_realm) return no(5);
            const BytecodeChunk& c = *exe->bytecode_chunk;
            if (construct) {
                Graph::CtorPlan plan;
                uint32_t n = 0;
                const bool sc = fn->stackless_constructible() && !fn->is_class_constructor();
                const bool ok = sc && ctor_plan(fn, plan, n, false);
                if (config().log) std::fprintf(stderr, "[opt] new at pc %u: constructible=%d plan=%d\n", in.pc, sc, ok);
                if (!ok) return no(6);
                out.push_back(fn);
                continue;
            }
            uint8_t v = c.inline_state;
            if (v & kInlineNever) return no(7);
            if (!(v & (kInlineOk | kInlineLeaf))) v = inline_classify(*exe, c, fn);
            if (v & kInlineNever || !(v & (kInlineOk | kInlineLeaf)) || (v & kInlineEnv)) return no(8);
            const bool strict = (v & kInlineLeaf) ? (fn->is_strict() || exe->fast_strict) : (v & kInlineStrict) != 0;
            if (!(v & kInlineLeaf) && g_.frame->ctx->is_strict_mode() != strict) return no(9);
            // A sloppy body that reads `this` boxes a primitive receiver: only a method call can promise an object.
            if (!strict && c.uses_this && static_cast<Op>(in.op) != Op::CallResolved) return no(10);
            uint32_t n = 0;
            if (!inline_scan(fn, c, n) || n > config().inline_insns) return no(11);
            out.push_back(fn);
        }
        return true;
    }
    return no(12);
}

// ---- natives ----

// The Math functions the tier computes itself (kMathFns), by name and arity.
constexpr struct { const char* name; uint8_t arity; } kMathNames[] = {
    {"floor", 1}, {"ceil", 1}, {"trunc", 1}, {"round", 1}, {"sqrt", 1}, {"abs", 1}, {"min", 2}, {"max", 2}, {"pow", 2}};
constexpr uint32_t kMathCount = sizeof(kMathNames) / sizeof(kMathNames[0]);

// Natives a unit calls without a frame, and what each may be given. The helper checks these before it calls, and
// hands the call back to the interpreter (a deoptimization) for anything else: a receiver or argument that could run
// script, or a call that throws. None of these changes anything before it can throw, so running the call again there is
// the same call. recv: 1 string, 2 array, 4 plain object, 8 anything (a static function).
constexpr struct { const char* name; uint8_t recv; bool any_arg; uint8_t max_args; } kNativeRules[] = {
    {"charCodeAt", 1, false, 1}, {"charAt", 1, false, 1}, {"codePointAt", 1, false, 1}, {"indexOf", 3, false, 2},
    {"lastIndexOf", 1, false, 2}, {"slice", 3, false, 2}, {"substring", 1, false, 2}, {"substr", 1, false, 2},
    {"toLowerCase", 1, false, 0}, {"toUpperCase", 1, false, 0}, {"trim", 1, false, 0}, {"startsWith", 1, false, 2},
    {"endsWith", 1, false, 2}, {"includes", 1, false, 2}, {"fromCharCode", 8, false, 2}, {"isArray", 8, true, 1},
    {"hasOwnProperty", 4, false, 1}, {"push", 2, false, 1}, {"pop", 2, false, 0}, {"sin", 8, false, 1}, {"cos", 8, false, 1},
    {"tan", 8, false, 1}, {"atan", 8, false, 1}, {"atan2", 8, false, 2}, {"exp", 8, false, 1}, {"log", 8, false, 1}};

inline bool Builder::native_call(const Insn& in) {
    if (!config().natives || !g_.frame) return false;
    const auto* fbk = F().chunk->ic_feedback.get();
    if (!fbk) return false;
    Function* fn = nullptr;
    for (const auto& cs : fbk->call_sites) if (cs.pc == in.pc && !cs.mega && cs.n == 1) fn = cs.fns[0];
    if (!fn || !fn->is_native()) return false;
    const uint8_t* code = F().chunk->code.data();
    const bool resolved = static_cast<Op>(in.op) == Op::CallResolved;
    const uint32_t callee_r = F().var_base + code[in.pc + 1];
    const uint32_t this_r = resolved ? F().var_base + code[in.pc + 2] : 0;
    const uint32_t start = F().var_base + code[in.pc + (resolved ? 3 : 2)];
    const uint32_t argc = code[in.pc + (resolved ? 4 : 3)];
    if (callee_r >= F().acc_var || (resolved && this_r >= F().acc_var) || start + argc > F().acc_var) return false;
    const std::string& name = fn->get_name();
    // Math functions are computed here, so the function must be the real one.
    for (uint32_t k = 0; k < kMathCount; k++) {
        if (name != kMathNames[k].name || argc != kMathNames[k].arity) continue;
        Object* global = g_.frame->ctx->get_global_object();
        const Value m = global ? global->get_property("Math") : Value();
        if (!m.is_object()) break;
        const Value mf = m.as_object()->get_property(name);
        if (!mf.is_function() || mf.as_function() != fn) break;
        if (!numeric_arg(value(start)) || (argc == 2 && !numeric_arg(value(start + 1)))) return false;
        guarded(K::CheckFn, Rep::None, tagged(value(callee_r)), -1, 1, reinterpret_cast<uint64_t>(fn));
        const int32_t a = num(value(start));
        const int32_t b = argc == 2 ? num(value(start + 1)) : -1;
        set(acc(), emit(K::MathF, Rep::F64, a, b, k));
        return true;
    }
    for (const auto& rule : kNativeRules) {
        if (name != rule.name || argc > rule.max_args) continue;
        auto d = std::make_unique<JitCode::OptUnit::NativeDesc>();
        d->fn = fn;
        d->argc = static_cast<uint8_t>(argc);
        d->recv = rule.recv;
        d->any_arg = rule.any_arg;
        JitCode::OptUnit::NativeDesc* dp = d.get();
        g_.natives.push_back(std::move(d));
        guarded(K::CheckFn, Rep::None, tagged(value(callee_r)), -1, 1, reinterpret_cast<uint64_t>(fn));
        const int32_t thisv = resolved ? tagged(value(this_r)) : const_of(Value());
        const int32_t a0 = argc >= 1 ? tagged(value(start)) : -1;
        const int32_t a1 = argc >= 2 ? tagged(value(start + 1)) : -1;
        set(acc(), guarded(K::CallNative, Rep::Tagged, thisv, a0, 0, reinterpret_cast<uint64_t>(dp), a1));
        return true;
    }
    return false;
}

// A block that has not run, going by what its property accesses have learned: a site that has seen no receiver of any
// kind belongs to code nobody has executed. (A receiver the caches do not learn from -- a Proxy -- looks the same, which
// costs speed and never correctness: the block is left to the interpreter.)
inline bool Builder::block_is_cold(int32_t fi, int32_t blk) const {
    const Func& fn = g_.funcs[fi];
    const Block& B = g_.b[blk];
    if (B.first_insn < 0 || B.synthetic) return false;
    const BytecodeChunk& c = *fn.chunk;
    for (int32_t i = B.first_insn; i <= B.last_insn; i++) {
        const Insn& in = fn.insns[i];
        const Op op = static_cast<Op>(in.op);
        if (op != Op::GetNamed && op != Op::GetNamedStar && op != Op::SetNamed) continue;
        const uint32_t fbi = read_u16(c.code.data(), in.pc + 4);
        if (fbi >= c.feedback.size()) continue;
        const FeedbackBody* fb = c.feedback[fbi].peek();
        if (!fb) return true;
        if (fb->count == 0 && fb->proto_count == 0 && !fb->array_length && !fb->prim_valid && !fb->own_desc_receiver &&
            !fb->mega && !fb->proto_mega && fb->transition_count == 0 && !fb->transition_mega)
            return true;
    }
    return false;
}

// What a constructor must look like for `new` to be inlined: a single block of straight-line code that does nothing outside
// the object it makes (its only stores are `this.name = value` adding properties along transitions the site has seen).
// Then every guard in it can leave through the `new` itself, and the object can be made with its final shape.
static bool fail_plan(int why) { if (config().log) std::fprintf(stderr, "[opt] ctor plan refused: %d\n", why); return false; }
// Whether `c` is exactly `function () { this.<name>.apply(this, arguments); }`; the name's index when it is.
inline bool apply_forwarder(const BytecodeChunk& c, uint32_t& name_index) {
    static constexpr Op kShape[] = {Op::LdaThisStar, Op::GetNamedStar, Op::LdarStar, Op::LdaFrameArgsMarker, Op::Star,
                                    Op::CallViaFunctionApply, Op::LdaUndefined, Op::Return};
    if (c.handlers && !c.handlers->empty()) return false;
    const uint8_t* code = c.code.data();
    const uint32_t size = static_cast<uint32_t>(c.code.size());
    uint32_t pc = 0;
    for (Op want : kShape) {
        if (pc >= size || code[pc] != static_cast<uint8_t>(want)) return false;
        if (want == Op::GetNamedStar) name_index = read_u16(code, pc + 2);
        pc += 1 + static_cast<uint32_t>(op_operand_bytes(want));
    }
    return pc == size;
}

inline bool Builder::ctor_plan(Function* fn, Graph::CtorPlan& plan, uint32_t& insns, bool make_desc) {
    const FunctionExecutable* exe = fn->get_executable().get();
    const Value pv = fn->constructor_prototype();
    Object* proto = pv.is_function() ? static_cast<Object*>(pv.as_function()) : pv.is_object() ? pv.as_object() : nullptr;
    if (!proto) return fail_plan(2);
    plan.inner = nullptr;
    uint32_t name_index = 0;
    if (apply_forwarder(*exe->bytecode_chunk, name_index)) {
        // The method the forwarder calls, as a plain data property in a shape slot of the prototype or an ancestor.
        const std::string& name = *exe->bytecode_chunk->names[name_index];
        for (Object* o = proto; o && !plan.inner; o = o->get_prototype_raw()) {
            Shape* sh = o->get_shape();
            if (!sh || o->has_descriptor_override(name)) return fail_plan(21);
            const int32_t slot = sh->find_slot(name);
            if (slot < 0) continue;
            if (sh->is_accessor_slot(name)) return fail_plan(22);
            const Value* sv = o->get_shape_slot_unchecked(static_cast<uint32_t>(slot));
            if (!sv || !sv->is_function()) return fail_plan(23);
            plan.inner = sv->as_function();
            plan.holder = o;
            plan.holder_slot = static_cast<uint32_t>(slot);
            plan.inner_bits = std::bit_cast<uint64_t>(*sv);
        }
        Function* in = plan.inner;
        if (!in || in->get_function_kind() != Function::FunctionKind::Plain || in->is_arrow() || !in->get_executable() ||
            !in->get_executable()->bytecode_chunk || in->get_executable()->owner_realm() != g_current_realm) return fail_plan(24);
        // `apply` must still be the real one: it is not called, and a change to it moves the descriptor epoch.
        const Value ap = in->get_property("apply");
        if (!ap.is_function() || !ap.as_function()->is_native() || ap.as_function()->get_name() != "apply") return fail_plan(25);
        plan.desc_epoch = Object::descriptor_epoch();
    }
    const BytecodeChunk& c = plan.inner ? *plan.inner->get_executable()->bytecode_chunk : *exe->bytecode_chunk;
    if ((c.handlers && !c.handlers->empty()) || c.register_count > kInlineMaxRegs) return fail_plan(1);
    const uint8_t* code = c.code.data();
    const uint32_t size = static_cast<uint32_t>(c.code.size());
    Shape* from = nullptr;
    Shape* cur = nullptr;
    insns = 0;
    plan.stores.clear();
    plan.proto_epoch = Object::proto_epoch();
    bool returned = false;
    std::vector<std::pair<uint32_t, uint32_t>> skipped;
    for (uint32_t pc = 0; pc < size;) {
        if (returned || code[pc] >= static_cast<uint8_t>(Op::kCount)) return fail_plan(3);
        const Op op = static_cast<Op>(code[pc]);
        if (op_unsupported(op) || op == Op::Throw) return fail_plan(4);
        // Forward branches only (a conditional expression computing a value): the code between a branch and its target runs
        // on some paths and not on others, and a store there would add its property on some paths only.
        if (const int bp = branch_offset_pos(op); bp >= 0) {
            const int32_t off = read_i16(code, pc + bp);
            if (off <= 0) return fail_plan(4);
            skipped.push_back({pc + 1 + static_cast<uint32_t>(op_operand_bytes(op)), pc + 1 + static_cast<uint32_t>(op_operand_bytes(op)) + static_cast<uint32_t>(off)});
        }
        if (op == Op::Return) returned = true;
        if (op == Op::SetNamed) {
            for (auto& [lo, hi] : skipped) if (pc >= lo && pc < hi) return fail_plan(10);
            const uint32_t fbi = read_u16(code, pc + 4);
            if (fbi >= c.feedback.size()) return fail_plan(5);
            const FeedbackBody& fb = c.feedback[fbi].read();
            if (fb.transition_mega || fb.transition_count == 0) return fail_plan(6);
            const FeedbackBody::TransitionEntry& te = fb.transitions[0];
            if (!te.from_shape || !te.to_shape || te.prototype != proto || te.proto_epoch != Object::proto_epoch()) return fail_plan(7);
            if (!cur) from = te.from_shape; else if (te.from_shape != cur) return fail_plan(8);
            cur = te.to_shape;
            plan.stores.push_back({pc, te.slot_index});
        }
        pc += 1 + static_cast<uint32_t>(op_operand_bytes(op));
        insns++;
    }
    if (!returned || plan.stores.empty()) return fail_plan(9);
    if (make_desc) {
        auto d = std::make_unique<JitCode::OptUnit::NewDesc>();
        d->from = from;
        d->to = cur;
        d->proto = proto;
        d->nslots = cur->slot_count();
        plan.desc = d.get();
        g_.descs.push_back(std::move(d));
    }
    return true;
}

inline bool Builder::inline_scan(const Function*, const BytecodeChunk& c, uint32_t& insns) {
    if ((c.handlers && !c.handlers->empty()) || c.register_count > kInlineMaxRegs) return false;
    const uint8_t* code = c.code.data();
    const uint32_t size = static_cast<uint32_t>(c.code.size());
    insns = 0;
    for (uint32_t pc = 0; pc < size;) {
        if (code[pc] >= static_cast<uint8_t>(Op::kCount)) return false;
        const Op op = static_cast<Op>(code[pc]);
        if (op_unsupported(op) || op == Op::Throw) return false;
        const uint32_t len = 1 + static_cast<uint32_t>(op_operand_bytes(op));
        const int pos = branch_offset_pos(op);
        if (pos >= 0 && read_i16(code, pc + pos) < 0) return false;      // no loops
        pc += len;
        insns++;
    }
    return true;
}

// Makes one inlined instance of `target` for the call `call` of function `fi`: its variables, its blocks (appended to the
// graph, the reachable ones listed in `added`), its returns wired to the block after the call. Returns its function
// number, or -1 (with nothing wired) when the callee does not fit the budgets.
inline int32_t Builder::make_instance(int32_t fi, const Insn& call, Function* target, std::vector<int32_t>& added) {
    const bool is_ctor = static_cast<Op>(call.op) == Op::Construct;
    uint32_t n = 0;
    Graph::CtorPlan plan;
    if (is_ctor && !ctor_plan(target, plan, n, true)) return -1;
    // The function whose body is inlined: the callee, or the method a forwarding constructor calls.
    Function* body = plan.inner ? plan.inner : target;
    const FunctionExecutable* exe = body->get_executable().get();
    const BytecodeChunk& c = *exe->bytecode_chunk;
    if (!is_ctor) inline_scan(target, c, n);
    const uint32_t nregs = std::max<uint32_t>(c.register_count, c.parameter_count);
    if (inline_insns_ + n > config().inline_total || inline_vars_ + nregs + 3 > config().inline_vars) return -1;
    inline_insns_ += n;
    inline_vars_ += nregs + 3;
    const bool leaf = is_ctor || (c.inline_state & kInlineLeaf) != 0;
    const bool strict = is_ctor || (leaf ? (target->is_strict() || exe->fast_strict) : (c.inline_state & kInlineStrict) != 0);

    Func f;
    f.chunk = &c;
    f.parent = fi;
    f.var_base = g_.nvars;
    f.nregs = nregs;
    f.acc_var = f.var_base + nregs;
    f.this_var = static_cast<int32_t>(f.acc_var + 1);
    f.fn_var = static_cast<int32_t>(f.acc_var + 2);
    f.call_pc = call.pc;
    f.call_len = call.len;
    f.cont_block = g_.funcs[fi].block_at[call.pc + call.len];
    f.leaf = leaf;
    f.exe = exe;
    f.fnp = target;          // what the call site is checked against: the forwarder itself, not the method it forwards to
    f.by_identity = c.uses_lookup_cache || plan.inner;
    f.lookup = c.lookup_cache.data();
    if (c.uses_lookup_cache) {
        PrivateFeedback* unused = nullptr;
        inline_instance_caches(c, body, f.lookup, unused);
    }
    f.sloppy_this = !strict && c.uses_this && !is_ctor;
    if (is_ctor) {
        f.ctor = true;
        f.plan = static_cast<int32_t>(g_.ctors.size());
        g_.ctors.push_back(plan);
    }
    g_.nvars += nregs + 3;
    g_.funcs.push_back(std::move(f));
    const int32_t ci = static_cast<int32_t>(g_.funcs.size() - 1);
    const size_t first = g_.b.size();
    if (!setup_func(ci, config().max_insns)) return -1;
    if (g_.funcs[ci].cont_block < 0) { fail("a call with nothing after it"); return -1; }
    g_.funcs[ci].entry_block = g_.funcs[ci].block_at[0];
    // Returns continue after the call.
    for (size_t bi = first; bi < g_.b.size(); bi++) {
        Block& B = g_.b[bi];
        if (B.first_insn >= 0 && static_cast<Op>(g_.funcs[ci].insns[B.last_insn].op) == Op::Return) B.succs.push_back(g_.funcs[ci].cont_block);
    }
    // The blocks the callee's first block reaches.
    std::vector<uint8_t> seen(g_.b.size(), 0);
    std::vector<int32_t> work{g_.funcs[ci].entry_block};
    const size_t added_before = added.size();
    while (!work.empty()) {
        const int32_t x = work.back();
        work.pop_back();
        if (x < static_cast<int32_t>(first) || seen[x]) continue;
        seen[x] = 1;
        added.push_back(x);
        for (int32_t s2 : g_.b[x].succs) work.push_back(s2);
    }
    for (size_t k = added_before; k < added.size(); k++)
        if (added[k] != g_.funcs[ci].entry_block && block_is_cold(ci, added[k])) g_.b[added[k]].cold = true;
    if (!leaf) {
        uint32_t words = 0, depth = 0;
        for (int32_t k = ci; k > 0; k = g_.funcs[k].parent) { words += inline_frame_words(g_.funcs[k].nregs); depth++; }
        g_.inl_words = std::max(g_.inl_words, words);
        g_.inl_depth = std::max(g_.inl_depth, depth);
        g_.inl_strict = strict;
    }
    g_.inl_count++;
    return ci;
}

// Replaces the call that ends `blk` by the callee's blocks: the call block jumps to the callee's first block, its
// returns go on to the block that follows the call. With several callees the call block starts a chain of identity
// checks, each leading to a block that sets up that callee's variables. Nothing is done when the callees do not fit the
// budgets.
inline bool Builder::expand_inline(int32_t blk, std::vector<int32_t>& added) {
    const int32_t fi = g_.b[blk].func;
    const Insn call = g_.funcs[fi].insns[g_.b[blk].last_insn];
    std::vector<Function*> targets;
    for (auto& cd : g_.funcs[fi].cand) if (cd.first == call.pc) targets.push_back(cd.second);
    if (targets.empty()) return false;
    // All or nothing: the budgets for every callee are checked before the first is made.
    {
        uint32_t insns = 0, vars = 0;
        for (Function* t : targets) {
            const BytecodeChunk& c = *t->get_executable()->bytecode_chunk;
            uint32_t n = 0;
            if (static_cast<Op>(call.op) == Op::Construct) { Graph::CtorPlan plan; if (!ctor_plan(t, plan, n, false)) return false; }
            else inline_scan(t, c, n);
            insns += n;
            vars += std::max<uint32_t>(c.register_count, c.parameter_count) + 3;
        }
        if (inline_insns_ + insns > config().inline_total || inline_vars_ + vars > config().inline_vars) return false;
    }
    if (targets.size() == 1) {
        const int32_t ci = make_instance(fi, call, targets[0], added);
        if (ci < 0) return false;
        g_.funcs[fi].sites.push_back({call.pc, ci});
        g_.b[blk].succs.assign(1, g_.funcs[ci].entry_block);
        return true;
    }
    std::vector<int32_t> cis, got;
    for (Function* t : targets) {
        const int32_t ci = make_instance(fi, call, t, got);
        if (ci < 0) return false;
        cis.push_back(ci);
    }
    const int32_t k = static_cast<int32_t>(cis.size());
    Func::PolySite ps;
    ps.pc = call.pc;
    ps.call = call;
    ps.callees = cis;
    const int32_t site_index = static_cast<int32_t>(g_.funcs[fi].polys.size());
    auto make = [&](int32_t kind, int32_t t) {
        const int32_t b = new_block(call.pc, call.pc);
        g_.b[b].func = fi;
        g_.b[b].poly_kind = kind;
        g_.b[b].poly_site = site_index;
        g_.b[b].poly_t = t;
        got.push_back(b);
        return b;
    };
    std::vector<int32_t> prep(k), disp(k, -1);
    for (int32_t t = 0; t < k; t++) {
        prep[t] = make(2, t);
        g_.b[prep[t]].succs.assign(1, g_.funcs[cis[t]].entry_block);
    }
    // disp[j] decides between target j and the rest; the call block itself decides target 0, the last needs no check of its own.
    for (int32_t j = 1; j + 1 < k; j++) disp[j] = make(1, j);
    auto next_of = [&](int32_t j) { return j + 1 < k - 1 ? disp[j + 1] : prep[k - 1]; };
    g_.b[blk].succs = {prep[0], k == 2 ? prep[1] : disp[1]};
    for (int32_t j = 1; j + 1 < k; j++) g_.b[disp[j]].succs = {prep[j], next_of(j)};
    g_.funcs[fi].polys.push_back(std::move(ps));
    for (int32_t x : got) added.push_back(x);
    return true;
}

// The identity test of target `t` of a polymorphic call, as a branch on the block being built.
inline bool Builder::poly_dispatch(int32_t site, int32_t t, bool& ended) {
    Func::PolySite& ps = F().polys[site];
    const uint8_t* code = F().chunk->code.data();
    const bool resolved = static_cast<Op>(ps.call.op) == Op::CallResolved;
    const uint32_t callee_r = F().var_base + code[ps.call.pc + 1];
    if (callee_r >= F().acc_var) return false;
    if (t == 0) ps.fv = tagged(value(callee_r));
    (void)resolved;
    const Func& C = g_.funcs[ps.callees[t]];
    const int32_t cond = emit(K::IsFn, Rep::Bool, ps.fv, -1, C.by_identity ? 1 : 0,
                              reinterpret_cast<uint64_t>(C.by_identity ? static_cast<const void*>(C.fnp) : C.exe));
    Block& B = g_.b[cb_];
    B.term.kind = T::Branch;
    B.term.cond = cond;
    B.term.t = B.succs[0];
    B.term.f = B.succs[1];
    ended = true;
    return true;
}

// The call itself: the callee's identity is checked (`guard`; a polymorphic call has told the callees apart already), the
// callee's variables are given their first values (its parameters from the arguments, the rest undefined).
inline bool Builder::inline_define(const Insn& in, int32_t ci, int32_t fv_in, bool guard) {
    const uint8_t* code = F().chunk->code.data();
    const Op op = static_cast<Op>(in.op);
    const bool resolved = op == Op::CallResolved;
    const bool construct = op == Op::Construct;
    const uint32_t callee_r = F().var_base + code[in.pc + 1];
    const uint32_t this_r = resolved ? F().var_base + code[in.pc + 2] : 0;
    const uint32_t start = F().var_base + code[in.pc + (resolved ? 3 : 2)];
    const uint32_t argc = code[in.pc + (resolved ? 4 : 3)];
    if (callee_r >= F().acc_var || (resolved && this_r >= F().acc_var) || start + argc > F().acc_var) return false;
    const Func C = g_.funcs[ci];
    const int32_t fv = fv_in >= 0 ? fv_in : tagged(value(callee_r));
    int32_t entry_state;
    if (guard) {
        const int32_t chk = guarded(K::CheckFn, Rep::None, fv, -1, C.by_identity ? 1 : 0, reinterpret_cast<uint64_t>(C.by_identity ? static_cast<const void*>(C.fnp) : C.exe));
        entry_state = g_.n[chk].state;
    } else {
        entry_state = snapshot();
    }
    int32_t thisv;
    if (construct) {
        // The constructor's object: made with the shape the stores will give it, so they are plain slot writes.
        const Graph::CtorPlan& plan = g_.ctors[C.plan];
        epoch_guard(0, plan.proto_epoch);
        thisv = guarded(K::NewObject, Rep::Tagged, fv, -1, 0, reinterpret_cast<uint64_t>(plan.desc));
        if (plan.inner) {
            // The forwarded-to method is still what the prototype holds, and `apply` is still the real one.
            epoch_guard(1, plan.desc_epoch);
            const int32_t held = emit(K::LoadHolder, Rep::Tagged, -1, -1, plan.holder_slot, reinterpret_cast<uint64_t>(plan.holder));
            guarded(K::CheckBits, Rep::None, held, -1, 0, plan.inner_bits);
        }
    } else {
        thisv = resolved ? tagged(value(this_r)) : const_of(Value());
    }
    if (C.sloppy_this) guarded(K::CheckObj, Rep::Ptr, thisv);
    const int32_t undef = const_of(Value());
    const uint32_t copy = std::min<uint32_t>(C.params, argc);
    for (uint32_t i = 0; i < C.nregs; i++) set(C.var_base + i, i < copy ? value(start + i) : undef);
    set(C.acc_var, undef);
    set(static_cast<uint32_t>(C.this_var), thisv);
    set(static_cast<uint32_t>(C.fn_var), fv);
    if (C.leaf) g_.funcs[ci].entry_state = entry_state;
    return true;
}

inline bool Builder::inline_enter(const Insn& in, int32_t ci, bool& ended) {
    if (!inline_define(in, ci, -1, true)) return false;
    Block& B = g_.b[cb_];
    B.term.kind = T::Jump;
    B.term.t = B.succs[0];
    ended = true;
    return true;
}

// Decodes one function, finds its basic blocks and links them inside it. The blocks are appended to the graph.
inline bool Builder::setup_func(int32_t fi, size_t max_insns) {
    const int32_t saved = cf_;
    cf_ = fi;
    struct Restore { Builder* b; int32_t f; ~Restore() { b->cf_ = f; } } restore{this, saved};
    const BytecodeChunk& c = *F().chunk;
    const uint8_t* code = c.code.data();
    const uint32_t size = static_cast<uint32_t>(c.code.size());
    if (c.handlers && !c.handlers->empty()) return fail("has try regions");

    // ---- decode ----
    F().insn_at.assign(size + 1, -1);
    for (uint32_t pc = 0; pc < size;) {
        const uint8_t raw = code[pc];
        if (raw >= static_cast<uint8_t>(Op::kCount)) return fail("invalid opcode");
        const Op op = static_cast<Op>(raw);
        if (op_unsupported(op)) return fail("suspending opcode");
        const uint32_t len = 1 + static_cast<uint32_t>(op_operand_bytes(op));
        if (pc + len > size) return fail("truncated instruction");
        F().insn_at[pc] = static_cast<int32_t>(F().insns.size());
        F().insns.push_back({pc, raw, static_cast<uint8_t>(len)});
        pc += len;
        total_insns_++;
        if (total_insns_ > max_insns) return fail("too many instructions");
    }
    if (F().insns.empty()) return fail("empty");
    compute_liveness();
    F().params = c.parameter_count;
    for (const Insn& in : F().insns) if (static_cast<Op>(in.op) == Op::LdaFrameArgsMarker) F().no_light = true;
    if (config().inl && g_.frame) {
        for (const Insn& in : F().insns) {
            const Op op = static_cast<Op>(in.op);
            if (op != Op::Call && op != Op::CallResolved && op != Op::Construct) continue;
            if (F().ctor) continue;        // a constructor that is inlined stays free of calls
            std::vector<Function*> ts;
            if (inline_targets(fi, in, ts)) for (Function* t : ts) F().cand.push_back({in.pc, t});
        }
    }

    // ---- leaders and loop headers ----
    std::vector<uint8_t> leader(size + 1, 0), header(size + 1, 0);
    leader[0] = 1;
    for (const Insn& in : F().insns) {
        const Op op = static_cast<Op>(in.op);
        const int pos = branch_offset_pos(op);
        const uint32_t next = in.pc + in.len;
        if (pos >= 0) {
            const int64_t t = static_cast<int64_t>(next) + i16(in.pc + pos);
            if (t < 0 || t >= size || F().insn_at[t] < 0) return fail("bad branch target");
            leader[t] = 1;
            if (t <= static_cast<int64_t>(in.pc)) header[t] = 1;
            if (next < size) leader[next] = 1;
        } else if (op == Op::Return || op == Op::Throw) {
            if (next < size) leader[next] = 1;
        }
    }
    for (auto& cd : F().cand) leader[F().insns[F().insn_at[cd.first]].pc + F().insns[F().insn_at[cd.first]].len] = 1;
    F().block_at.assign(size + 1, -1);
    const size_t first_block = g_.b.size();
    {
        uint32_t start = 0;
        for (uint32_t pc = 1; pc <= size; pc++) {
            if (pc == size || leader[pc]) {
                if (F().insn_at[start] < 0) return fail("leader inside an instruction");
                const int32_t id = new_block(start, pc);
                g_.b[id].func = fi;
                F().block_at[start] = id;
                g_.b[id].loop_header = header[start] != 0;
                g_.b[id].first_insn = F().insn_at[start];
                // Last instruction of the range: the one before `pc`.
                int32_t li = F().insn_at[start];
                while (li + 1 < static_cast<int32_t>(F().insns.size()) && F().insns[li + 1].pc < pc) li++;
                g_.b[id].last_insn = li;
                start = pc;
            }
        }
    }
    // ---- successors ----
    for (size_t bi = first_block; bi < g_.b.size(); bi++) {
        Block& B = g_.b[bi];
        const Insn& last = F().insns[B.last_insn];
        const Op op = static_cast<Op>(last.op);
        const int pos = branch_offset_pos(op);
        const uint32_t next = last.pc + last.len;
        auto blk_at = [&](int64_t pc) -> int32_t { return (pc >= 0 && pc < size) ? F().block_at[pc] : -1; };
        if (pos >= 0) {
            const int64_t t = static_cast<int64_t>(next) + i16(last.pc + pos);
            const int32_t tb = blk_at(t);
            if (tb < 0) return fail("branch into the middle of a block");
            B.succs.push_back(tb);
            if (op != Op::Jump) {
                const int32_t fb = blk_at(next);
                if (fb < 0) return fail("conditional branch falls off the end");
                B.succs.push_back(fb);
            }
        } else if (op == Op::Return || op == Op::Throw) {
            // no successors
        } else {
            const int32_t fb = blk_at(next);
            if (fb < 0) return fail("falls off the end");
            B.succs.push_back(fb);
        }
    }
    return true;
}

inline bool Builder::build(const std::vector<uint32_t>& entry_pcs, size_t max_insns) {
    const BytecodeChunk& c = *g_.chunk;
    const uint32_t size = static_cast<uint32_t>(c.code.size());
    {
        Func root;
        root.chunk = &c;
        root.var_base = 0;
        root.nregs = c.register_count;
        root.acc_var = c.register_count;
        g_.funcs.push_back(std::move(root));
        g_.nvars = static_cast<uint32_t>(c.register_count) + 1;
        g_.acc_var = c.register_count;
        cf_ = 0;
    }
    if (!setup_func(0, max_insns)) return false;

    // ---- the region: the loops around each requested entry ----
    // Code outside the loop nest has not run (or is not what is hot), so it is not compiled: an edge
    // that leaves the region becomes an exit block that writes the variables back and resumes the
    // interpreter at the target.
    std::vector<uint8_t> reach(g_.b.size(), 0);
    const size_t nb0 = g_.b.size();
    std::vector<std::vector<int32_t>> bpreds(nb0);
    for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++)
        for (int32_t s2 : g_.b[i].succs) bpreds[s2].push_back(i);
    std::vector<uint32_t> pcs = entry_pcs;
    if (g_.fn_entry) {
        // The whole function: everything the first block reaches.
        std::vector<int32_t> work{F().block_at[0]};
        while (!work.empty()) {
            const int32_t x = work.back();
            work.pop_back();
            if (reach[x]) continue;
            reach[x] = 1;
            for (int32_t s2 : g_.b[x].succs) work.push_back(s2);
        }
    }
    for (uint32_t pc : (g_.fn_entry ? std::vector<uint32_t>() : pcs)) {
        if (pc >= size || F().block_at[pc] < 0) return fail("entry is not a block start");
        const int32_t H = F().block_at[pc];
        std::vector<uint8_t> fwd(nb0, 0);
        {
            std::vector<int32_t> work{H};
            while (!work.empty()) {
                const int32_t x = work.back();
                work.pop_back();
                if (fwd[x]) continue;
                fwd[x] = 1;
                for (int32_t s2 : g_.b[x].succs) work.push_back(s2);
            }
        }
        for (int32_t T = 0; T < static_cast<int32_t>(nb0); T++) {
            if (!fwd[T] || !g_.b[T].loop_header) continue;
            // Natural loop of T: T and everything that reaches a back edge into it without passing through it.
            std::vector<uint8_t> in(nb0, 0);
            in[T] = 1;
            std::vector<int32_t> work;
            for (int32_t src : bpreds[T])
                if (fwd[src] && g_.b[src].start_pc >= g_.b[T].start_pc && !in[src]) { in[src] = 1; work.push_back(src); }
            while (!work.empty()) {
                const int32_t x = work.back();
                work.pop_back();
                for (int32_t p : bpreds[x])
                    if (fwd[p] && !in[p]) { in[p] = 1; work.push_back(p); }
            }
            if (!in[H]) continue;
            for (size_t i = 0; i < nb0; i++) if (in[i]) reach[i] = 1;
        }
    }
    for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++) {
        if (reach[i] && g_.b[i].loop_header && std::find(pcs.begin(), pcs.end(), g_.b[i].start_pc) == pcs.end())
            pcs.push_back(g_.b[i].start_pc);
    }
    for (uint32_t pc : pcs) {
        if (!reach[F().block_at[pc]]) return fail("entry is not in a loop");
        const int32_t e = new_block(0, 0);
        g_.b[e].synthetic = true;
        g_.b[e].osr_pc = static_cast<int32_t>(pc);
        g_.b[e].succs.push_back(F().block_at[pc]);
        g_.b[e].sealed = true;
        g_.entries.push_back(e);
    }
    reach.resize(g_.b.size(), 0);
    // ---- cold code ----
    // Blocks whose property accesses have never seen a receiver have not run. They are left out of the region (an edge
    // into one becomes an exit to the interpreter), and so is everything only they lead to.
    if (config().cold) {
        std::vector<uint8_t> cold(nb0, 0), seen(g_.b.size(), 0);
        for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++) cold[i] = reach[i] && block_is_cold(0, i);
        std::vector<int32_t> work;
        for (int32_t e : g_.entries) for (int32_t s2 : g_.b[e].succs) work.push_back(s2);
        while (!work.empty()) {
            const int32_t x = work.back();
            work.pop_back();
            if (x >= static_cast<int32_t>(nb0) || seen[x] || !reach[x]) continue;
            // An entry's own block stays whatever it looks like.
            bool is_entry_target = false;
            for (int32_t e : g_.entries) if (g_.b[e].succs[0] == x) is_entry_target = true;
            if (cold[x] && !is_entry_target) continue;
            seen[x] = 1;
            for (int32_t s2 : g_.b[x].succs) work.push_back(s2);
        }
        for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++) reach[i] = seen[i];
    }
    // ---- inlining ----
    if (config().inl && g_.frame) {
        auto is_site = [&](int32_t b) {
            const Block& B = g_.b[b];
            if (B.last_insn < 0 || B.synthetic) return false;
            const Func& fn = g_.funcs[B.func];
            const uint32_t pc = fn.insns[B.last_insn].pc;
            // The block after the call has to be part of the region (it may have been left out as cold): the callee's
            // returns go there.
            const int32_t cont = fn.block_at[pc + fn.insns[B.last_insn].len];
            if (cont < 0 || cont >= static_cast<int32_t>(reach.size()) || !reach[cont]) return false;
            for (auto& cd : fn.cand) if (cd.first == pc) return true;
            return false;
        };
        std::vector<int32_t> work;
        for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++) if (reach[i] && is_site(i)) work.push_back(i);
        while (!work.empty()) {
            const int32_t blk = work.back();
            work.pop_back();
            std::vector<int32_t> added;
            const bool ok = expand_inline(blk, added);
            if (g_.why) return false;
            if (!ok) continue;
            reach.resize(g_.b.size(), 0);
            for (int32_t x : added) reach[x] = 1;
            for (int32_t x : added) if (is_site(x)) work.push_back(x);
        }
        for (Block& B : g_.b) B.def.resize(g_.nvars, kUnset);
    }
    reach.resize(g_.b.size(), 0);
    // Exit blocks.
    for (int32_t i = 0; i < static_cast<int32_t>(nb0); i++) {
        if (!reach[i]) continue;
        for (size_t k = 0; k < g_.b[i].succs.size(); k++) {
            const int32_t t = g_.b[i].succs[k];
            if (t >= static_cast<int32_t>(nb0) || reach[t]) continue;
            const int32_t x = new_block(g_.b[t].start_pc, g_.b[t].start_pc);
            g_.b[x].synthetic = true;
            g_.b[x].exit_pc = static_cast<int32_t>(g_.b[t].start_pc);
            g_.b[i].succs[k] = x;
            reach.push_back(1);
        }
    }
    reach.resize(g_.b.size(), 1);

    // ---- split the edges that a conversion or a phi move may need to sit on ----
    {
        std::vector<int32_t> npred(g_.b.size(), 0);
        for (int32_t i = 0; i < static_cast<int32_t>(g_.b.size()); i++) {
            if (!reach[i] && !g_.b[i].synthetic) continue;
            for (int32_t s : g_.b[i].succs) npred[s]++;
        }
        const int32_t original = static_cast<int32_t>(g_.b.size());
        for (int32_t i = 0; i < original; i++) {
            if (!reach[i] && !g_.b[i].synthetic) continue;
            if (g_.b[i].succs.size() < 2) continue;
            for (size_t k = 0; k < g_.b[i].succs.size(); k++) {
                const int32_t s = g_.b[i].succs[k];
                if (npred[s] < 2) continue;
                const int32_t l = new_block(g_.b[s].start_pc, g_.b[s].start_pc);
                g_.b[l].func = g_.b[s].func;           // its pc is in that function's code
                g_.b[l].synthetic = true;
                g_.b[l].succs.push_back(s);
                g_.b[l].sealed = false;
                g_.b[i].succs[k] = l;
            }
        }
    }
    // Predecessor lists, from scratch, in block order.
    for (Block& B : g_.b) B.preds.clear();
    for (int32_t i = 0; i < static_cast<int32_t>(g_.b.size()); i++) {
        if (!reach[i] && !g_.b[i].synthetic) continue;
        for (int32_t s : g_.b[i].succs) g_.b[s].preds.push_back(i);
    }
    // Blocks that are only successors of unreachable synthetic leftovers cannot exist; entries have no preds.

    g_.rpo = compute_rpo();
    if (g_.rpo.empty()) return fail("no blocks");

    // ---- SSA ----
    memver_.assign(g_.nvars, 0);
    for (int32_t blk : g_.rpo) {
        g_.b[blk].reachable = true;
        if (g_.b[blk].preds.empty()) g_.b[blk].sealed = true;
    }
    for (int32_t blk : g_.rpo) {
        Block& B = g_.b[blk];
        if (!B.sealed && B.filled_preds == static_cast<int32_t>(B.preds.size())) seal(blk);
        if (!translate_block(blk)) return false;
        g_.b[blk].built = true;
        for (int32_t s : g_.b[blk].succs) {
            Block& S = g_.b[s];
            S.filled_preds++;
            if (!S.sealed && S.filled_preds == static_cast<int32_t>(S.preds.size())) seal(s);
        }
    }
    for (int32_t blk : g_.rpo)
        if (!g_.b[blk].sealed) return fail("unsealed block");

    finalize();
    return g_.why == nullptr;
}

// ===========================================================================
// After construction: phis, representations, lowering, dead code
// ===========================================================================

// Every variable the interpreter may read when control leaves a block for a successor has its
// definition at the end of that block looked up now, so that the deoptimization states the later
// passes take (a guard on an incoming edge resumes at the successor's first instruction) find them
// and never have to build a phi after the fact.
inline void Builder::complete_defs() {
    for (int32_t blk : g_.rpo) {
        const std::vector<int32_t> succs = g_.b[blk].succs;
        for (int32_t s : succs) {
            std::vector<uint32_t> vars;
            state_vars(g_.b[s].func, g_.b[s].start_pc, vars);
            for (uint32_t v : vars) read_var(v, blk);
        }
    }
    frozen_ = true;
}

inline void Builder::finalize() {
    complete_defs();
    remove_trivial_phis();
    materialize_mem_operands();
    select_representations();
    lower();
    optimize();
    eliminate_dead();
    // A region that spends its time in steps is slower than the interpreter (every step writes the whole
    // register file back and reads it again), so it is not worth having.
    size_t steps = 0, insns = 0;
    for (int32_t blk : g_.rpo) {
        const Block& B = g_.b[blk];
        if (B.first_insn >= 0 && !B.synthetic) insns += static_cast<size_t>(B.last_insn - B.first_insn + 1);
        for (int32_t id : B.nodes) if (g_.n[id].op == K::Step) steps++;
    }
    if (config().log) {
        for (int32_t blk : g_.rpo)
            for (int32_t id : g_.b[blk].nodes)
                if (g_.n[id].op == K::Step) std::fprintf(stderr, "[opt] step %s\n", op_name(static_cast<Op>(F().chunk->code[g_.n[id].aux])));
    }
    if (insns && steps * 100 > static_cast<size_t>(config().max_step_percent) * insns) fail("too many steps");
    // Entering and leaving a unit costs a few dozen instructions; a body that is not much longer than that, with
    // nothing inlined into it, is cheaper to interpret.
    if (g_.fn_entry && !g_.why && g_.inl_count == 0 && insns < config().entry_min_insns) {
        bool loops = false;
        for (int32_t blk : g_.rpo) if (g_.b[blk].loop_header) loops = true;
        if (!loops) fail("too small to enter");
    }
    // A call the unit has to leave to a step runs on the native stack and through the slow call path, which costs
    // more than the interpreter's own stackless call. A loop pays for that with the iterations around it; a whole
    // function that is called often does not.
    if (g_.fn_entry && !g_.why && !config().allow_calls) {
        for (int32_t blk : g_.rpo)
            for (int32_t id : g_.b[blk].nodes) {
                if (g_.n[id].op != K::Step) continue;
                const Op op = static_cast<Op>(F().chunk->code[g_.n[id].aux]);
                if (op == Op::Call || op == Op::CallResolved || op == Op::Construct || op == Op::CallViaFunctionCall || op == Op::CallViaFunctionApply) { fail("calls remain"); return; }
            }
    }
}

inline void Builder::remove_trivial_phis() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int32_t blk : g_.rpo) {
            for (int32_t id : g_.b[blk].nodes) {
                if (g_.n[id].op != K::Phi || g_.n[id].dead) continue;
                const size_t before = g_.n.size();
                try_remove_trivial(id);
                if (g_.n[id].dead || g_.n.size() != before) changed = true;
                if (g_.n.size() != before) break;   // a load was inserted into this block's list: restart the block
            }
        }
    }
}

inline void Builder::materialize_mem_operands() {
    for (int32_t blk : g_.rpo) {
        const std::vector<int32_t> nodes = g_.b[blk].nodes;
        for (int32_t id : nodes) {
            if (g_.n[id].op != K::Phi || g_.n[id].dead) continue;
            // Indices only: emitting a load grows g_.n and would invalidate a reference to the phi.
            for (size_t i = 0; i < g_.n[id].ins.size(); i++) {
                int32_t d = resolve(g_.n[id].ins[i]);
                if (d == kMem) {
                    const int32_t p = g_.b[blk].preds[i];
                    const uint32_t v = g_.n[id].aux;
                    d = emit_in(p, v == g_.acc_var ? K::LoadAcc : K::LoadReg, Rep::Tagged, -1, -1, v, kAnyVersion);
                }
                g_.n[id].ins[i] = d;
            }
        }
    }
}

inline void Builder::select_representations() {
    std::vector<int32_t> phis;
    for (int32_t blk : g_.rpo)
        for (int32_t id : g_.b[blk].nodes)
            if (g_.n[id].op == K::Phi && !g_.n[id].dead) phis.push_back(id);
    std::vector<uint8_t> cand(g_.n.size(), 0), disq(g_.n.size(), 0);
    // A phi is a number only when every way into it is one that is known to carry a number: a value the
    // code computed as one, or -- on the requested entry only -- a register that holds one right now.
    // A value reloaded from the register bank after a step could be anything; speculating on it makes a
    // guard that fails every other iteration, so such a phi stays tagged and its uses check for themselves.
    auto entry_is_number = [&](const Node& on) {
        if (!g_.frame || (on.op != K::LoadReg && on.op != K::LoadAcc)) return false;
        const Block& B = g_.b[on.blk];
        if (!B.synthetic || B.osr_pc != static_cast<int32_t>(g_.entry_pc)) return false;
        const Value& v = on.op == K::LoadAcc ? g_.frame->acc : g_.frame->regs[on.aux];
        return v.is_number();
    };
    for (int32_t p : phis) {
        if (avoid_ && avoid_->phis.count({g_.b[g_.n[p].blk].start_pc, g_.n[p].aux})) { disq[p] = 1; continue; }
        for (int32_t o : g_.n[p].ins) {
            o = resolve(o);
            const Node& on = g_.n[o];
            if (on.op == K::Phi) continue;
            if (on.rep == Rep::F64) cand[p] = 1;
            else if (entry_is_number(on)) { /* neutral: a number at the entry */ }
            else disq[p] = 1;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (int32_t p : phis) {
            for (int32_t o : g_.n[p].ins) {
                o = resolve(o);
                if (g_.n[o].op != K::Phi) continue;
                if (cand[o] && !cand[p]) { cand[p] = 1; changed = true; }
                if (disq[o] && !disq[p]) { disq[p] = 1; changed = true; }
            }
        }
    }
    for (int32_t p : phis) g_.n[p].rep = (cand[p] && !disq[p]) ? Rep::F64 : Rep::Tagged;
}

inline void Builder::lower() {
    // Operands first: every reference goes through the replacement map.
    auto fix = [&](int32_t& x) { if (x >= 0) x = resolve(x); };
    // 1. pseudo-ops.
    for (int32_t blk : g_.rpo) {
        std::vector<int32_t> out;
        for (int32_t id : g_.b[blk].nodes) {
            Node& nd = g_.n[id];
            fix(nd.a); fix(nd.b); fix(nd.c);
            if (nd.dead) continue;
            switch (nd.op) {
            case K::ToNum: {
                const Node& x = g_.n[nd.a];
                if (x.rep == Rep::F64) { g_.repl[id] = nd.a; nd.dead = true; continue; }
                nd.op = K::CheckNum;
                break;
            }
            case K::ToTagged: {
                const Node& x = g_.n[nd.a];
                if (x.rep == Rep::Tagged) { g_.repl[id] = nd.a; nd.dead = true; continue; }
                nd.op = x.rep == Rep::F64 ? K::BoxF64 : K::BoxBool;
                break;
            }
            case K::ToBool: {
                const Node& x = g_.n[nd.a];
                if (x.rep == Rep::Bool) { g_.repl[id] = nd.a; nd.dead = true; continue; }
                nd.op = x.rep == Rep::F64 ? K::TruthyF : K::Truthy;
                break;
            }
            default: break;
            }
            out.push_back(id);
        }
        g_.b[blk].nodes = std::move(out);
    }
    // 2. phi operands: convert on the incoming edge.
    for (int32_t blk : g_.rpo) {
        const std::vector<int32_t> nodes = g_.b[blk].nodes;
        for (int32_t id : nodes) {
            if (g_.n[id].op != K::Phi || g_.n[id].dead) continue;
            for (size_t i = 0; i < g_.n[id].ins.size(); i++) {
                int32_t o = resolve(g_.n[id].ins[i]);
                const Rep want = g_.n[id].rep;
                const Rep have = g_.n[o].rep;
                const int32_t p = g_.b[blk].preds[i];
                if (have != want) {
                    cb_ = p;
                    const uint32_t pc = g_.b[blk].start_pc;
                    if (want == Rep::F64) {
                        // The incoming value is a Tagged one: it has to be a number.
                        const int32_t st = snapshot_at(p, g_.b[blk].func, pc, static_cast<int32_t>(g_.n[id].aux));
                        o = emit_in(p, K::CheckNum, Rep::F64, o);
                        g_.n[o].state = st;
                    } else {
                        o = emit_in(p, have == Rep::F64 ? K::BoxF64 : K::BoxBool, Rep::Tagged, o);
                    }
                }
                g_.n[id].ins[i] = o;
            }
        }
    }
    // 3. final operand resolution (terminators, states).
    for (int32_t blk : g_.rpo) {
        Block& B = g_.b[blk];
        fix(B.term.cond);
        for (int32_t id : B.nodes) { fix(g_.n[id].a); fix(g_.n[id].b); fix(g_.n[id].c); for (int32_t& o : g_.n[id].ins) fix(o); }
    }
    for (State& s : g_.states) {
        std::vector<std::pair<uint32_t, int32_t>> keep;
        for (auto& [v, d] : s.defs) {
            const int32_t r = resolve(d);
            if (r < 0) continue;
            if (is_own_load(v, r)) continue;
            keep.push_back({v, r});
        }
        s.defs = std::move(keep);
    }
}

inline void Builder::eliminate_dead() {
    std::vector<uint8_t> live(g_.n.size(), 0);
    std::vector<int32_t> work;
    auto use = [&](int32_t x) {
        if (x >= 0 && !live[x]) { live[x] = 1; work.push_back(x); }
    };
    std::vector<uint8_t> state_used(g_.states.size(), 0);
    for (int32_t blk : g_.rpo) {
        Block& B = g_.b[blk];
        use(B.term.cond);
        for (int32_t id : B.nodes)
            if (!is_pure_removable(g_.n[id].op)) use(id);
    }
    while (!work.empty()) {
        const int32_t x = work.back();
        work.pop_back();
        const Node& nd = g_.n[x];
        use(nd.a);
        use(nd.b);
        use(nd.c);
        for (int32_t o : nd.ins) use(o);
        if (nd.state >= 0 && !state_used[nd.state]) {
            state_used[nd.state] = 1;
            for (auto& [v, d] : g_.states[nd.state].defs) use(d);
        }
    }
    for (int32_t blk : g_.rpo) {
        std::vector<int32_t> out;
        for (int32_t id : g_.b[blk].nodes)
            if (live[id]) out.push_back(id);
        g_.b[blk].nodes = std::move(out);
    }
}


// ===========================================================================
// Optimization passes
// ===========================================================================

inline uint32_t js_to_int32(double d) {
    if (!std::isfinite(d)) return 0;
    d = std::trunc(d);
    d = std::fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return static_cast<uint32_t>(d);
}
inline uint64_t bits_of_double(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double double_of_bits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
// The boxed form of a double, as Value stores it.
inline uint64_t boxed_bits(double d) {
    const uint64_t b = bits_of_double(d);
    if (((b << 1) >> 53) != 0x7FF) return b;
    if (b == 0x7FF0000000000000ull) return 0x7FFB000000000000ull;
    if (b == 0xFFF0000000000000ull) return 0x7FFA000000000000ull;
    return 0x7FF8000000000000ull;
}

inline void Builder::fold() {
    auto cf = [&](int32_t id, double& out) {
        const Node& nd = g_.n[id];
        if (nd.op != K::ConstF64) return false;
        out = double_of_bits(nd.imm);
        return true;
    };
    auto ci = [&](int32_t id, uint32_t& out) {
        const Node& nd = g_.n[id];
        if (nd.op != K::ConstI32) return false;
        out = static_cast<uint32_t>(nd.imm);
        return true;
    };
    auto make_f = [&](Node& nd, double d) { nd.op = K::ConstF64; nd.rep = Rep::F64; nd.a = nd.b = nd.c = -1; nd.imm = bits_of_double(d); nd.state = -1; };
    auto make_i = [&](Node& nd, uint32_t v) { nd.op = K::ConstI32; nd.rep = Rep::I32; nd.a = nd.b = nd.c = -1; nd.imm = v; nd.state = -1; };
    for (int32_t blk : g_.rpo) {
        for (int32_t id : g_.b[blk].nodes) {
            Node& nd = g_.n[id];
            if (nd.dead) continue;
            nd.a = resolve(nd.a); nd.b = resolve(nd.b); nd.c = resolve(nd.c);
            double x, y;
            uint32_t u, v;
            switch (nd.op) {
            case K::ToI32:
                if (cf(nd.a, x)) { make_i(nd, js_to_int32(x)); break; }
                if (g_.n[nd.a].op == K::I32ToF64 || g_.n[nd.a].op == K::U32ToF64) { g_.repl[id] = g_.n[nd.a].a; nd.dead = true; }
                break;
            case K::I32ToF64: if (ci(nd.a, u)) make_f(nd, static_cast<double>(static_cast<int32_t>(u))); break;
            case K::U32ToF64: if (ci(nd.a, u)) make_f(nd, static_cast<double>(u)); break;
            case K::AndI: case K::OrI: case K::XorI: case K::ShlI: case K::SarI: case K::ShrI:
                if (ci(nd.a, u) && ci(nd.b, v)) {
                    uint32_t r = 0;
                    switch (nd.op) {
                    case K::AndI: r = u & v; break;
                    case K::OrI: r = u | v; break;
                    case K::XorI: r = u ^ v; break;
                    case K::ShlI: r = u << (v & 31); break;
                    case K::SarI: r = static_cast<uint32_t>(static_cast<int32_t>(u) >> (v & 31)); break;
                    default: r = u >> (v & 31); break;
                    }
                    make_i(nd, r);
                }
                break;
            case K::NotI: if (ci(nd.a, u)) make_i(nd, ~u); break;
            case K::AddF: case K::SubF: case K::MulF: case K::DivF:
                if (cf(nd.a, x) && cf(nd.b, y)) {
                    make_f(nd, nd.op == K::AddF ? x + y : nd.op == K::SubF ? x - y : nd.op == K::MulF ? x * y : x / y);
                }
                break;
            case K::NegF: if (cf(nd.a, x)) make_f(nd, -x); break;
            case K::CheckNum:
                if (g_.n[nd.a].op == K::ConstF64) { g_.repl[id] = nd.a; nd.dead = true; }
                break;
            case K::ToIndex:
                if (cf(nd.a, x)) {
                    if (x >= 0 && x <= 2147483647.0 && x == std::floor(x)) make_i(nd, static_cast<uint32_t>(x));
                } else if (g_.n[nd.a].op == K::I32ToF64) {
                    nd.op = K::NonNegI32;
                    nd.a = g_.n[nd.a].a;
                }
                break;
            case K::BoxF64:
                if (cf(nd.a, x)) { nd.op = K::ConstTagged; nd.rep = Rep::Tagged; nd.imm = boxed_bits(x); nd.a = -1; }
                break;
            case K::BoxBool:
                if (g_.n[nd.a].op == K::ConstBool) {
                    nd.op = K::ConstTagged; nd.rep = Rep::Tagged;
                    nd.imm = 0x7FF2000000000000ull + (g_.n[nd.a].imm << 48);
                    nd.a = -1;
                }
                break;
            case K::NotBool:
                if (g_.n[nd.a].op == K::ConstBool) { nd.op = K::ConstBool; nd.rep = Rep::Bool; nd.imm = g_.n[nd.a].imm ^ 1; nd.a = -1; }
                break;
            default: break;
            }
        }
        g_.b[blk].term.cond = resolve(g_.b[blk].term.cond);
    }
}

// Common subexpressions, over the dominator tree. Pure values are shared wherever one dominates the
// other. Guards and loads depend on memory, so they are shared only inside one "epoch": a run of
// code with no step and no store between the two (a step can change anything; a store can change
// what a load would read). A block with several predecessors starts a new epoch.
inline void Builder::cse() {
    const size_t nb = g_.b.size();
    std::vector<int32_t> rpo_num(nb, -1);
    for (size_t i = 0; i < g_.rpo.size(); i++) rpo_num[g_.rpo[i]] = static_cast<int32_t>(i);
    const int32_t kRoot = static_cast<int32_t>(nb);
    std::vector<int32_t> idom(nb + 1, -1);
    // Entries hang off a virtual root.
    for (int32_t e : g_.entries) idom[e] = kRoot;
    auto num = [&](int32_t b) { return b == kRoot ? -1 : rpo_num[b]; };
    auto intersect = [&](int32_t a, int32_t b) {
        while (a != b) {
            while (num(a) > num(b)) a = idom[a];
            while (num(b) > num(a)) b = idom[b];
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int32_t blk : g_.rpo) {
            if (g_.b[blk].preds.empty()) continue;
            int32_t nd = -1;
            for (int32_t p : g_.b[blk].preds) {
                if (idom[p] == -1 && p != kRoot) continue;      // not processed yet
                nd = nd == -1 ? p : intersect(p, nd);
            }
            if (nd != -1 && idom[blk] != nd) { idom[blk] = nd; changed = true; }
        }
    }
    std::vector<std::vector<int32_t>> kids(nb + 1);
    for (int32_t blk : g_.rpo) if (idom[blk] >= 0) kids[idom[blk]].push_back(blk);

    struct Key {
        K op; int32_t a, b, c; uint32_t aux; uint64_t imm; uint64_t epoch;
        bool operator==(const Key& o) const { return op == o.op && a == o.a && b == o.b && c == o.c && aux == o.aux && imm == o.imm && epoch == o.epoch; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = static_cast<uint64_t>(k.op) * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<uint64_t>(static_cast<uint32_t>(k.a)) << 32 | static_cast<uint32_t>(k.b)) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= (static_cast<uint64_t>(static_cast<uint32_t>(k.c)) << 32 | k.aux) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= (k.imm ^ (k.epoch * 0xC2B2AE3D27D4EB4Full)) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };
    std::unordered_map<Key, int32_t, KeyHash> table;
    uint64_t next_epoch = 1;
    std::vector<uint64_t> end_epoch(nb, 0);
    auto kind_of = [](K op) {   // 0 not shared, 1 pure, 2 memory-dependent
        switch (op) {
        case K::ConstTagged: case K::ConstF64: case K::ConstBool: case K::ConstI32:
        case K::CheckNum: case K::BoxF64: case K::BoxBool: case K::ToI32: case K::I32ToF64: case K::U32ToF64:
        case K::AddF: case K::SubF: case K::MulF: case K::DivF: case K::NegF: case K::ModF:
        case K::AndI: case K::OrI: case K::XorI: case K::ShlI: case K::SarI: case K::ShrI: case K::NotI:
        case K::LtF: case K::GtF: case K::LeF: case K::GeF: case K::EqF: case K::NeF: case K::EqTag: case K::NeTag:
        case K::NotBool: case K::TruthyF: case K::Truthy: case K::NotTdz: case K::CheckObj: case K::ToIndex: case K::NonNegI32: case K::CheckFn: case K::IsNullish: case K::CheckNotNullish: case K::CheckBits: case K::IsFn:
            return 1;
        case K::LoadReg: case K::LoadAcc: case K::LoadSlot: case K::LoadElem: case K::ArrayLen: case K::LoadEnv:
        case K::LoadThis: case K::CheckShape: case K::CheckDense: case K::CheckProto: case K::CheckEpoch: case K::LoadHolder: case K::LoadPoly:
            return 2;
        default: return 0;
        }
    };
    auto breaks_memory = [](K op) {
        return op == K::Step || op == K::StoreSlot || op == K::StoreElem || op == K::StoreEnv || op == K::StoreReg || op == K::StoreAcc ||
               op == K::CallNative;
    };
    struct Frame2 { int32_t blk; size_t kid; std::vector<Key> added; };
    std::vector<Frame2> stack;
    auto enter = [&](int32_t blk) {
        // Epoch at the start of the block.
        uint64_t epoch = next_epoch++;
        const Block& B = g_.b[blk];
        if (B.preds.size() == 1 && idom[blk] == B.preds[0]) epoch = end_epoch[B.preds[0]];
        stack.push_back({blk, 0, {}});
        Frame2& fr = stack.back();
        for (int32_t id : g_.b[blk].nodes) {
            Node& nd = g_.n[id];
            if (nd.dead) continue;
            nd.a = resolve(nd.a); nd.b = resolve(nd.b); nd.c = resolve(nd.c);
            const int kind = kind_of(nd.op);
            if (kind) {
                Key k{nd.op, nd.a, nd.b, nd.c, nd.aux, nd.imm, kind == 2 ? epoch : 0};
                auto it = table.find(k);
                if (it != table.end() && !g_.n[it->second].dead) {
                    g_.repl[id] = it->second;
                    nd.dead = true;
                    continue;
                }
                if (it == table.end()) { table.emplace(k, id); fr.added.push_back(k); }
            }
            if (breaks_memory(nd.op)) epoch = next_epoch++;
        }
        end_epoch[blk] = epoch;
    };
    for (int32_t e : kids[kRoot]) {
        enter(e);
        while (!stack.empty()) {
            Frame2& fr = stack.back();
            if (fr.kid < kids[fr.blk].size()) {
                const int32_t child = kids[fr.blk][fr.kid++];
                enter(child);
            } else {
                for (const Key& k : fr.added) table.erase(k);
                stack.pop_back();
            }
        }
    }
    // Keep the lists free of the nodes that were folded away.
    for (int32_t blk : g_.rpo) {
        std::vector<int32_t> out;
        for (int32_t id : g_.b[blk].nodes) if (!g_.n[id].dead) out.push_back(id);
        g_.b[blk].nodes = std::move(out);
    }
    for (int32_t blk : g_.rpo) {
        g_.b[blk].term.cond = resolve(g_.b[blk].term.cond);
        for (int32_t id : g_.b[blk].nodes) {
            Node& nd = g_.n[id];
            nd.a = resolve(nd.a); nd.b = resolve(nd.b); nd.c = resolve(nd.c);
            for (int32_t& o : nd.ins) o = resolve(o);
        }
    }
    for (State& st : g_.states)
        for (auto& [v, d] : st.defs) d = resolve(d);
}

// ===========================================================================
// Dump
// ===========================================================================

inline void dump(const Graph& g, FILE* out) {
    std::fprintf(out, "[opt] graph: %zu blocks, %zu nodes, %zu states, %u vars\n", g.rpo.size(), g.n.size(), g.states.size(), g.nvars);
    for (int32_t blk : g.rpo) {
        const Block& B = g.b[blk];
        std::fprintf(out, "  b%d [%u,%u)%s%s preds:", blk, B.start_pc, B.end_pc, B.loop_header ? " loop" : "", B.synthetic ? " syn" : "");
        for (int32_t p : B.preds) std::fprintf(out, " b%d", p);
        if (B.osr_pc >= 0) std::fprintf(out, " (entry pc %d)", B.osr_pc);
        std::fprintf(out, "\n");
        for (int32_t id : B.nodes) {
            const Node& nd = g.n[id];
            std::fprintf(out, "    v%d:%s = %s", id, rname(nd.rep), kname(nd.op));
            if (nd.op == K::Phi) {
                std::fprintf(out, "[r%u](", nd.aux);
                for (size_t i = 0; i < nd.ins.size(); i++) std::fprintf(out, "%sv%d", i ? ", " : "", nd.ins[i]);
                std::fprintf(out, ")");
            } else {
                if (nd.a >= 0) std::fprintf(out, " v%d", nd.a);
                if (nd.b >= 0) std::fprintf(out, ", v%d", nd.b);
                if (nd.c >= 0) std::fprintf(out, ", v%d", nd.c);
                if (nd.op == K::LoadSlot || nd.op == K::StoreSlot) std::fprintf(out, " slot %u", nd.aux);
                if (nd.op == K::CheckShape) std::fprintf(out, " 0x%llx", static_cast<unsigned long long>(nd.imm));
                if (nd.op == K::LoadReg || nd.op == K::StoreReg) std::fprintf(out, " [%u]", nd.aux);
                if (nd.op == K::Step) std::fprintf(out, " [%u] %s", nd.aux, op_name(static_cast<Op>(g.chunk->code[nd.aux])));
                if (nd.op == K::ConstF64) { double d; std::memcpy(&d, &nd.imm, 8); std::fprintf(out, " %g", d); }
                if (nd.op == K::ConstTagged || nd.op == K::ConstBool) std::fprintf(out, " 0x%llx", static_cast<unsigned long long>(nd.imm));
                if (nd.state >= 0) std::fprintf(out, "  ;state%d@%u(%zu)", nd.state, g.states[nd.state].pc, g.states[nd.state].defs.size());
            }
            std::fprintf(out, "\n");
        }
        const Term& t = B.term;
        switch (t.kind) {
        case T::Jump: std::fprintf(out, "    -> b%d\n", t.t); break;
        case T::Branch: std::fprintf(out, "    if v%d -> b%d else b%d\n", t.cond, t.t, t.f); break;
        case T::Return: std::fprintf(out, "    return v%d\n", t.cond); break;
        case T::StepBranch: std::fprintf(out, "    stepbranch v%d -> b%d (pc %u) / b%d (pc %u)\n", t.cond, t.t, t.tpc, t.f, t.fpc); break;
        case T::Exit: std::fprintf(out, "    exit\n"); break;
        case T::Bail: std::fprintf(out, "    bail to pc %u\n", t.tpc); break;
        default: std::fprintf(out, "    <no terminator>\n"); break;
        }
    }
}


// ===========================================================================
// Register allocation and code generation (x86-64, System V)
// ===========================================================================

#if defined(QUANTA_JIT_ARCH_X64) || defined(QUANTA_JIT_ARCH_A64)
#define QUANTA_OPT_BACKEND 1
#ifdef QUANTA_JIT_ARCH_A64
constexpr int kPushBytes = 16;     // push/pop keep sp 16-aligned: a slot is two words
#else
constexpr int kPushBytes = 8;
#endif

// Helpers the generated code calls from its cold paths. noexcept: no C++ exception unwinds through
// generated code.
inline QUANTA_JIT_ABI uint64_t helper_truthy(uint64_t bits) noexcept {
    Value v;
    std::memcpy(&v, &bits, 8);
    return v.to_boolean() ? 1 : 0;
}
inline QUANTA_JIT_ABI double math_floor(double x) noexcept { return std::floor(x); }
inline QUANTA_JIT_ABI double math_ceil(double x) noexcept { return std::ceil(x); }
inline QUANTA_JIT_ABI double math_trunc(double x) noexcept { return std::trunc(x); }
inline QUANTA_JIT_ABI double math_sqrt(double x) noexcept { return std::sqrt(x); }
inline QUANTA_JIT_ABI double math_abs(double x) noexcept { return std::fabs(x); }
inline QUANTA_JIT_ABI double math_round(double x) noexcept {
    if (x >= -0.5 && x < 0) return -0.0;
    const double r = std::floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}
inline QUANTA_JIT_ABI double math_min(double a, double b) noexcept {
    if (a != a || b != b) return std::numeric_limits<double>::quiet_NaN();
    if (a == b) { uint64_t x, y; std::memcpy(&x, &a, 8); std::memcpy(&y, &b, 8); x |= y; std::memcpy(&a, &x, 8); return a; }   // -0 beats +0
    return a < b ? a : b;
}
inline QUANTA_JIT_ABI double math_max(double a, double b) noexcept {
    if (a != a || b != b) return std::numeric_limits<double>::quiet_NaN();
    if (a == b) { uint64_t x, y; std::memcpy(&x, &a, 8); std::memcpy(&y, &b, 8); x &= y; std::memcpy(&a, &x, 8); return a; }   // +0 beats -0
    return a > b ? a : b;
}
inline QUANTA_JIT_ABI double math_pow(double x, double y) noexcept {
    if (y != y) return std::numeric_limits<double>::quiet_NaN();
    if (y == 0) return 1;
    if ((x == 1 || x == -1) && std::isinf(y)) return std::numeric_limits<double>::quiet_NaN();
    return std::pow(x, y);
}
inline const void* const kMathFns[] = {
    reinterpret_cast<const void*>(&math_floor), reinterpret_cast<const void*>(&math_ceil), reinterpret_cast<const void*>(&math_trunc),
    reinterpret_cast<const void*>(&math_round), reinterpret_cast<const void*>(&math_sqrt), reinterpret_cast<const void*>(&math_abs),
    reinterpret_cast<const void*>(&math_min), reinterpret_cast<const void*>(&math_max), reinterpret_cast<const void*>(&math_pow)};

// A whitelisted native called with its arguments in a small array (see kNativeRules). All ones is "no": the receiver or
// an argument is not one it may be given, or the call threw; the exception is dropped and the unit deoptimizes to the
// call, which the interpreter then makes (and throws again, if it throws).
inline QUANTA_JIT_ABI uint64_t helper_call_native(VM::Frame* f, const JitCode::OptUnit::NativeDesc* d, uint64_t this_bits, const uint64_t* argv) noexcept {
    constexpr uint64_t kNo = ~0ull;
    Value th;
    std::memcpy(&th, &this_bits, 8);
    Value args[2];
    for (uint32_t i = 0; i < d->argc; i++) std::memcpy(&args[i], &argv[i], 8);
    const bool recv_ok = (d->recv & 8) || ((d->recv & 1) && th.is_string()) ||
                         ((d->recv & 2) && th.is_object() && th.as_object()->is_array()) ||
                         ((d->recv & 4) && th.is_object() && th.as_object()->get_type() == Object::ObjectType::Ordinary);
    if (!recv_ok) return kNo;
    if (!d->any_arg)
        for (uint32_t i = 0; i < d->argc; i++) {
            const Value& v = args[i];
            if (!(v.is_number() || v.is_string() || v.is_boolean() || v.is_undefined() || v.is_null())) return kNo;
        }
    Context& ctx = *f->ctx;
    const Value r = d->fn->call_register_args(ctx, std::span<const Value>(args, d->argc), th);
    if (ctx.has_exception()) { ctx.clear_exception(); return kNo; }
    uint64_t bits;
    std::memcpy(&bits, &r, 8);
    return bits == kNo ? kNo - 1 : bits;
}

// An inlined `new`: the constructor's object, already of the shape its stores would give it. 0 when the constructor's
// prototype or the fresh object's shape is not what the unit was built for (the caller deoptimizes to run the `new`).
inline QUANTA_JIT_ABI uint64_t helper_new_object(uint64_t fn_bits, const JitCode::OptUnit::NewDesc* d) noexcept {
    Value fv;
    std::memcpy(&fv, &fn_bits, 8);
    Function* fn = fv.as_function();
    const Value pv = fn->constructor_prototype();
    Object* proto = pv.is_function() ? static_cast<Object*>(pv.as_function()) : pv.is_object() ? pv.as_object() : nullptr;
    if (proto != d->proto) return 0;
    Object* self = fn->new_construct_object(proto);
    if (self->get_shape() != d->from) return 0;
    self->reserve_property_slots(d->nslots);
    self->adopt_literal_shape(d->to, d->nslots);
    const Value r(self);
    uint64_t bits;
    std::memcpy(&bits, &r, 8);
    return bits;
}
// An object literal whose shape the interpreter has learned (Op::CreateObjectT): 0 when the template has lost it.
inline QUANTA_JIT_ABI uint64_t helper_new_literal(const BytecodeChunk::IcFeedback::LiteralTemplate* t, Context* ctx) noexcept {
    if (!t->shape) return 0;
    Object* obj;
    if (t->count <= 4) {
        obj = ObjectFactory::create_object_with_slots((t->count + 1u) & ~1u).release();
    } else {
        obj = ObjectFactory::create_object().release();
        obj->reserve_property_slots(t->count);
    }
    obj->adopt_literal_shape(t->shape, t->count);
    Engine::fixup_new_object_realm(obj, ctx);
    const Value r(obj);
    uint64_t bits;
    std::memcpy(&bits, &r, 8);
    return bits;
}
inline QUANTA_JIT_ABI void helper_barrier(Object* obj) noexcept { Collector::write_barrier(obj); }
inline QUANTA_JIT_ABI void helper_barrier_value(Object* obj, uint64_t bits) noexcept {
    Value v;
    std::memcpy(&v, &bits, 8);
    Collector::write_barrier_value(obj, v);
}
inline QUANTA_JIT_ABI void helper_barrier_env(Environment* env, uint64_t bits) noexcept {
    Value v;
    std::memcpy(&v, &bits, 8);
    Collector::write_barrier_env_for(env, v);
}
inline QUANTA_JIT_ABI double helper_fmod(double a, double b) noexcept { return std::fmod(a, b); }
// A load from the current environment whose predicted slot was not there: the walk by name the
// interpreter's general handler makes, over declarative environments. 0 for anything else (a hole, an
// object environment, an unbound name): the interpreter will say what it is.
inline QUANTA_JIT_ABI uint64_t helper_env_load(Frame* f, const std::string* key, uint64_t* out) noexcept {
    Value v;
    if (!frame_lexical_env(*f)->try_read_declarative_chain(key, v)) return 0;
    std::memcpy(out, &v, 8);
    return 1;
}
inline QUANTA_JIT_ABI void helper_poll(JitRt* rt) noexcept {
    rt->sp_counter = g_cfg.safepoint_reload;
    Collector::safepoint();
}

class Emitter {
public:
    explicit Emitter(Graph& g) : g_(g) {}
    bool emit(std::vector<uint8_t>& out, std::vector<std::pair<uint32_t, uint32_t>>& entries);
    const char* why() const { return why_; }
    // For each state a guard can deoptimize to: its pc and the kind of the first guard that uses it.
    std::vector<std::tuple<uint32_t, const char*, int32_t>> guard_kinds;

private:
    Graph& g_;
    Assembler a_;
    const char* why_ = nullptr;

    enum LK : uint8_t { kNone = 0, kGpr, kXmm, kStack };
    struct Loc { LK kind = kNone; uint8_t reg = 0; int32_t slot = -1; };
    std::vector<Loc> loc_;
    std::vector<int32_t> pos_, bstart_, bend_;
    std::vector<int32_t> order_;
    int32_t nslots_ = 0;
    int32_t native_args_ = -1;     // first of the four frame slots a native call's receiver and arguments are laid out in
    uint32_t used_gpr_ = 0, used_xmm_ = 0;     // masks of registers the allocator handed out
    std::deque<Label> labels_;
    std::vector<Label*> blk_label_;
    std::vector<Label*> state_label_;
    Label epilogue_, step_exit_;
    std::vector<std::function<void()>> cold_;
    int32_t frame_bytes_ = 0;
    const char* cur_kind_ = "?";

    bool fail(const char* w) { if (!why_) why_ = w; return false; }
    Label& new_label() { labels_.emplace_back(); return labels_.back(); }

    // ---- register allocation ----
    static constexpr Reg kGprPool[] = {RSI, RDI, R8, R9, R10, R13, R15, RBP};     // R11 is a fourth scratch
    static constexpr int kXmmFirst = 2, kXmmLast = 13;
    bool needs_loc(int32_t id) const {
        const Node& n = g_.n[id];
        if (n.dead) return false;
        switch (n.op) {
        case K::ConstTagged: case K::ConstF64: case K::ConstBool: case K::ConstI32: return false;
        default: break;
        }
        return n.rep != Rep::None;
    }
    bool allocate();

    // ---- operand access ----
    Mem slot_mem(const Loc& l) const { return mem(RSP, l.slot * 8); }
    uint64_t const_bits(const Node& n) const { return n.imm; }
    Reg use_g(int32_t id, Reg scratch);
    Xmm use_x(int32_t id, Xmm scratch);
    Reg def_g(int32_t id, Reg scratch) { const Loc& l = loc_[id]; return l.kind == kGpr ? static_cast<Reg>(l.reg) : scratch; }
    Xmm def_x(int32_t id, Xmm scratch) { const Loc& l = loc_[id]; return l.kind == kXmm ? static_cast<Xmm>(l.reg) : scratch; }
    void commit_g(int32_t id, Reg r) { const Loc& l = loc_[id]; if (l.kind == kStack) a_.mov_store(slot_mem(l), r); }
    void commit_x(int32_t id, Xmm x) { const Loc& l = loc_[id]; if (l.kind == kStack) a_.movsd_store(slot_mem(l), x); }

    // ---- emission ----
    Label& deopt_label(int32_t state);
    void stress_guard(int32_t state);
    void emit_node(int32_t id);
    void emit_term(int32_t blk, size_t layout_index);
    void emit_phi_moves(int32_t from, int32_t to);
    void emit_box_inline(Reg d, Xmm s);      // d = boxed(s), using RCX
    // After a store of `val` into the object `o`: tells the collector unless the value cannot hold a cell.
    void emit_barrier_check(int32_t val_node, Reg o, Reg v);
    // The same for a store into the environment whose address is in RAX.
    void emit_env_barrier_check(int32_t val_node, Reg v);
    void emit_deopt_stub(int32_t state);
    void save_live(int32_t& bytes);
    void restore_live(int32_t bytes);
    void emit_prologue_epilogue();
    // A call into C++: the target in RAX, and the shadow space the Microsoft convention wants below the arguments.
    void call_c(const void* fn) {
        a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(fn));
        if (abi::kShadow) a_.sub_imm(RSP, abi::kShadow);
        a_.call_reg(RAX);
        if (abi::kShadow) a_.add_imm(RSP, abi::kShadow);
    }
    int32_t xmm_save_off_ = 0;     // Microsoft convention: where xmm6-xmm15 are kept while the code runs
};

constexpr Reg Emitter::kGprPool[];

inline bool Emitter::allocate() {
    const size_t nn = g_.n.size();
    pos_.assign(nn, -1);
    bstart_.assign(g_.b.size(), -1);
    bend_.assign(g_.b.size(), -1);
    order_ = g_.rpo;
    int32_t p = 0;
    for (int32_t blk : order_) {
        bstart_[blk] = p;
        for (int32_t id : g_.b[blk].nodes) { pos_[id] = g_.n[id].op == K::Phi ? bstart_[blk] : p; p += 2; }
        bend_[blk] = p;
        p += 2;
    }
    const size_t words = (nn + 63) / 64;
    std::vector<std::vector<uint64_t>> live_in(g_.b.size(), std::vector<uint64_t>(words, 0)),
                                       live_out(g_.b.size(), std::vector<uint64_t>(words, 0));
    auto setb = [&](std::vector<uint64_t>& v, int32_t i) { v[i >> 6] |= 1ull << (i & 63); };
    auto clrb = [&](std::vector<uint64_t>& v, int32_t i) { v[i >> 6] &= ~(1ull << (i & 63)); };
    auto getb = [&](const std::vector<uint64_t>& v, int32_t i) { return (v[i >> 6] >> (i & 63)) & 1; };

    // Operands of a node that matter for liveness (including the values a guard's frame state needs).
    auto for_uses = [&](int32_t id, auto&& fn) {
        const Node& n = g_.n[id];
        if (n.op == K::Phi) return;
        if (n.a >= 0 && needs_loc(n.a)) fn(n.a);
        if (n.b >= 0 && needs_loc(n.b)) fn(n.b);
        if (n.c >= 0 && needs_loc(n.c)) fn(n.c);
        if (n.state >= 0) for (auto& [v, d] : g_.states[n.state].defs) if (needs_loc(d)) fn(d);
    };
    auto phi_operand = [&](int32_t succ, int32_t pred) -> std::vector<int32_t> {
        std::vector<int32_t> r;
        const Block& S = g_.b[succ];
        size_t idx = 0;
        for (; idx < S.preds.size(); idx++) if (S.preds[idx] == pred) break;
        for (int32_t id : S.nodes) {
            if (g_.n[id].op != K::Phi) break;
            if (!g_.n[id].dead && idx < g_.n[id].ins.size() && needs_loc(g_.n[id].ins[idx])) r.push_back(g_.n[id].ins[idx]);
        }
        return r;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            const int32_t blk = *it;
            std::vector<uint64_t> out(words, 0);
            for (int32_t s : g_.b[blk].succs) {
                for (size_t w = 0; w < words; w++) out[w] |= live_in[s][w];
                for (int32_t o : phi_operand(s, blk)) setb(out, o);
            }
            std::vector<uint64_t> live = out;
            const Term& t = g_.b[blk].term;
            if ((t.kind == T::Branch || t.kind == T::Return || t.kind == T::StepBranch) && t.cond >= 0 && needs_loc(t.cond)) setb(live, t.cond);
            const auto& nodes = g_.b[blk].nodes;
            for (auto nit = nodes.rbegin(); nit != nodes.rend(); ++nit) {
                const int32_t id = *nit;
                if (needs_loc(id)) clrb(live, id);
                for_uses(id, [&](int32_t u) { setb(live, u); });
            }
            if (live != live_in[blk]) { live_in[blk] = live; changed = true; }
            if (out != live_out[blk]) { live_out[blk] = out; changed = true; }
        }
    }

    // Intervals: one range per value, no holes.
    std::vector<int32_t> start(nn, INT32_MAX), end(nn, -1);
    std::vector<int32_t> ids;
    for (int32_t blk : order_) {
        for (int32_t id : g_.b[blk].nodes) {
            if (!needs_loc(id)) continue;
            start[id] = pos_[id];
            end[id] = pos_[id];
            ids.push_back(id);
        }
    }
    for (int32_t blk : order_) {
        for (int32_t id : ids) {
            if (getb(live_in[blk], id)) { start[id] = std::min(start[id], bstart_[blk]); end[id] = std::max(end[id], bstart_[blk]); }
            if (getb(live_out[blk], id)) end[id] = std::max(end[id], bend_[blk]);
        }
        for (int32_t id : g_.b[blk].nodes) {
            for_uses(id, [&](int32_t u) { end[u] = std::max(end[u], pos_[id]); });
        }
        const Term& t = g_.b[blk].term;
        if ((t.kind == T::Branch || t.kind == T::Return || t.kind == T::StepBranch) && t.cond >= 0 && needs_loc(t.cond))
            end[t.cond] = std::max(end[t.cond], bend_[blk]);
    }
    // Nothing may be live across a step: the callee clobbers every register and the tier does not
    // save them (a step is a full barrier, so the builder never lets a value cross one). Checked on
    // real liveness, not on the intervals, which have no holes and so overlap steps they are dead at.
    for (int32_t blk : order_) {
        std::vector<uint64_t> live = live_out[blk];
        const Term& t = g_.b[blk].term;
        if ((t.kind == T::Branch || t.kind == T::Return || t.kind == T::StepBranch) && t.cond >= 0 && needs_loc(t.cond)) setb(live, t.cond);
        const auto& nodes = g_.b[blk].nodes;
        for (auto nit = nodes.rbegin(); nit != nodes.rend(); ++nit) {
            const int32_t id = *nit;
            if (g_.n[id].op == K::Step && !g_.n[id].light) {
                std::vector<uint64_t> after = live;
                clrb(after, id);
                for (size_t w = 0; w < words; w++)
                    if (after[w]) return fail("value live across a step");
            }
            if (needs_loc(id)) clrb(live, id);
            for_uses(id, [&](int32_t u) { setb(live, u); });
        }
    }

    loc_.assign(nn, Loc{});
    std::sort(ids.begin(), ids.end(), [&](int32_t x, int32_t y) { return start[x] != start[y] ? start[x] < start[y] : x < y; });
    std::vector<int32_t> active_g, active_x;
    uint32_t free_g = 0, free_x = 0;
    for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) free_g |= 1u << i;
    for (int r = kXmmFirst; r <= kXmmLast; r++) free_x |= 1u << r;
    auto gpr_index = [&](uint8_t reg) { for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) if (kGprPool[i] == reg) return static_cast<int>(i); return -1; };
    for (int32_t id : ids) {
        const bool fp = g_.n[id].rep == Rep::F64;
        auto& active = fp ? active_x : active_g;
        uint32_t& freem = fp ? free_x : free_g;
        // Expire.
        for (size_t i = 0; i < active.size();) {
            const int32_t o = active[i];
            if (end[o] < start[id]) {
                if (loc_[o].kind == kGpr) freem |= 1u << gpr_index(loc_[o].reg);
                else if (loc_[o].kind == kXmm) freem |= 1u << loc_[o].reg;
                active[i] = active.back();
                active.pop_back();
            } else i++;
        }
        if (freem) {
            const int bit = __builtin_ctz(freem);
            freem &= ~(1u << bit);
            if (fp) { loc_[id] = {kXmm, static_cast<uint8_t>(bit), -1}; used_xmm_ |= 1u << bit; }
            else { loc_[id] = {kGpr, static_cast<uint8_t>(kGprPool[bit]), -1}; used_gpr_ |= 1u << bit; }
            active.push_back(id);
            continue;
        }
        // Spill the interval that ends last.
        int32_t victim = id;
        for (int32_t o : active) if (end[o] > end[victim]) victim = o;
        if (victim == id) {
            loc_[id] = {kStack, 0, nslots_++};
        } else {
            loc_[id] = loc_[victim];
            loc_[victim] = {kStack, 0, nslots_++};
            active.erase(std::find(active.begin(), active.end(), victim));
            active.push_back(id);
        }
    }
    for (int32_t blk : order_)
        for (int32_t id : g_.b[blk].nodes)
            if (!g_.n[id].dead && g_.n[id].op == K::CallNative && native_args_ < 0) { native_args_ = nslots_; nslots_ += 4; }
#ifdef QUANTA_JIT_ARCH_A64
    frame_bytes_ = ((nslots_ * 8 + 15) / 16) * 16;
#else
    frame_bytes_ = 8 + ((nslots_ * 8 + 15) / 16) * 16;
#endif
#ifdef QUANTA_JIT_WIN64
    xmm_save_off_ = frame_bytes_ - 8;      // 16-aligned: frame_bytes_ is 8 more than a multiple of 16
    frame_bytes_ += 10 * 16;               // xmm6-xmm15 are the callee's to preserve
#endif
    return true;
}

inline Reg Emitter::use_g(int32_t id, Reg scratch) {
    const Node& n = g_.n[id];
    switch (n.op) {
    case K::ConstTagged: case K::ConstBool: case K::ConstI32:
        a_.mov_imm64(scratch, n.imm);
        return scratch;
    default: break;
    }
    const Loc& l = loc_[id];
    if (l.kind == kGpr) return static_cast<Reg>(l.reg);
    if (l.kind == kStack) { a_.mov_load(scratch, slot_mem(l)); return scratch; }
    why_ = "operand without a location";
    return scratch;
}

inline Xmm Emitter::use_x(int32_t id, Xmm scratch) {
    const Node& n = g_.n[id];
    if (n.op == K::ConstF64) {
        a_.mov_imm64(RDX, n.imm);
        a_.movq_to_xmm(scratch, RDX);
        return scratch;
    }
    const Loc& l = loc_[id];
    if (l.kind == kXmm) return static_cast<Xmm>(l.reg);
    if (l.kind == kStack) { a_.movsd_load(scratch, slot_mem(l)); return scratch; }
    why_ = "operand without a location";
    return scratch;
}

inline void Emitter::stress_guard(int32_t state) {
    if (!config().stress) return;
    Label& hit = new_label();
    a_.sub32_mem_imm(mem(R14, static_cast<int32_t>(offsetof(JitRt, stress))), 1);
    a_.jcc(CC_LE, hit);
    Label* dl = &deopt_label(state);
    cold_.push_back([this, &hit, dl]() {
        a_.bind(hit);
        a_.mov_store32_imm(mem(R14, static_cast<int32_t>(offsetof(JitRt, stress))), config().stress);
        a_.jmp(*dl);
    });
}

inline Label& Emitter::deopt_label(int32_t state) {
    if (!state_label_[state]) state_label_[state] = &new_label();
    if (guard_kinds.size() <= static_cast<size_t>(state)) guard_kinds.resize(state + 1, {0, nullptr, -1});
    if (!std::get<1>(guard_kinds[state])) guard_kinds[state] = {g_.states[state].pc, cur_kind_, g_.states[state].edge_var};
    return *state_label_[state];
}

// d = the boxed form of the double in `s`. Clobbers RCX.
inline void Emitter::emit_box_inline(Reg d, Xmm s) {
    Label ok, pinf, ninf;
    a_.movq_from_xmm(d, s);
    a_.mov(RCX, d);
    a_.add(RCX, RCX);
    a_.shr_imm(RCX, 53);
    a_.cmp32_imm(RCX, 0x7FF);
    a_.jcc(CC_NE, ok);
    a_.mov_imm64(RCX, 0x7FF0000000000000ull);
    a_.cmp(d, RCX);
    a_.jcc(CC_E, pinf);
    a_.mov_imm64(RCX, 0xFFF0000000000000ull);
    a_.cmp(d, RCX);
    a_.jcc(CC_E, ninf);
    a_.mov_imm64(d, 0x7FF8000000000000ull);
    a_.jmp(ok);
    a_.bind(pinf);
    a_.mov_imm64(d, 0x7FFB000000000000ull);
    a_.jmp(ok);
    a_.bind(ninf);
    a_.mov_imm64(d, 0x7FFA000000000000ull);
    a_.bind(ok);
}

inline void Emitter::emit_barrier_check(int32_t val_node, Reg o, Reg v) {
    const Node& vn = g_.n[val_node];
    if (vn.op == K::BoxF64 || vn.op == K::BoxBool) return;      // a number or a boolean holds no cell
    if (vn.op == K::ConstTagged) {
        Value c;
        std::memcpy(&c, &vn.imm, 8);
        if (c.is_undefined() || c.is_null()) return;
    }
    Label& cold = new_label();
    Label& back = new_label();
    a_.mov(RCX, v);
    a_.shr_imm(RCX, 52);
    a_.cmp32_imm(RCX, 0x7FF);
    a_.jcc(CC_E, cold);       // everything but a finite double asks the collector
    a_.bind(back);
    cold_.push_back([this, o, v, &cold, &back]() {
        a_.bind(cold);
        int32_t bytes;
        save_live(bytes);
        a_.push(o);
        a_.push(v);
        a_.pop(abi::A1);
        a_.pop(abi::A0);
        call_c(reinterpret_cast<const void*>(&helper_barrier_value));
        restore_live(bytes);
        a_.jmp(back);
    });
}

inline void Emitter::emit_env_barrier_check(int32_t val_node, Reg v) {
    const Node& vn = g_.n[val_node];
    if (vn.op == K::BoxF64 || vn.op == K::BoxBool) return;
    if (vn.op == K::ConstTagged) {
        Value c;
        std::memcpy(&c, &vn.imm, 8);
        if (c.is_undefined() || c.is_null()) return;
    }
    Label& cold = new_label();
    Label& back = new_label();
    a_.mov(RCX, v);
    a_.shr_imm(RCX, 52);
    a_.cmp32_imm(RCX, 0x7FF);
    a_.jcc(CC_E, cold);
    a_.bind(back);
    cold_.push_back([this, v, &cold, &back]() {
        a_.bind(cold);
        a_.push(RAX);          // the environment
        a_.push(v);
        int32_t bytes;
        save_live(bytes);
        a_.mov_load(abi::A1, mem(RSP, bytes));
        a_.mov_load(abi::A0, mem(RSP, bytes + kPushBytes));
        call_c(reinterpret_cast<const void*>(&helper_barrier_env));
        restore_live(bytes);
        a_.pop(v);
        a_.pop(RAX);
        a_.jmp(back);
    });
}

inline void Emitter::emit_deopt_stub(int32_t state) {
    const State& st = g_.states[state];
    for (auto& [v, d] : st.defs) {
        const Node& nd = g_.n[d];
        Reg val = RAX;
        switch (nd.rep) {
        case Rep::F64: emit_box_inline(RAX, use_x(d, XMM0)); break;
        case Rep::Bool: {
            const Reg b = use_g(d, RDX);
            a_.mov(RCX, b);
            a_.shl_imm(RCX, 48);
            a_.mov_imm64(RAX, 0x7FF2000000000000ull);
            a_.add(RAX, RCX);
            break;
        }
        default: val = use_g(d, RAX); break;
        }
        if (v == g_.acc_var) a_.mov_store(mem(RBX, static_cast<int32_t>(kOffAcc)), val);
        else if (v > g_.acc_var) {
            // A variable of an inlined callee: the interpreter frame it belongs to does not exist yet.
            a_.mov_load(RCX, mem(R14, static_cast<int32_t>(offsetof(JitRt, inl_scratch))));
            a_.mov_store(mem(RCX, static_cast<int32_t>((v - g_.acc_var - 1) * sizeof(Value))), val);
        }
        else a_.mov_store(mem(R12, static_cast<int32_t>(v * sizeof(Value))), val);
    }
    a_.mov_store32_imm(mem(R14, static_cast<int32_t>(offsetof(JitRt, last_deopt))), static_cast<uint32_t>(state));
    a_.mov_store32_imm(mem(RBX, static_cast<int32_t>(kOffPc)), st.pc);
    a_.mov_imm32(RAX, kStatusDeopt);
    a_.jmp(epilogue_);
}

// Saves the registers the allocator uses that a C call would clobber, onto the stack, keeping it 16-aligned.
inline void Emitter::save_live(int32_t& bytes) {
    int n = 0;
    uint32_t gm = 0;
    for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) {
        const Reg r = kGprPool[i];
        if ((used_gpr_ >> i & 1) && (r == RSI || r == RDI || r == R8 || r == R9 || r == R10 || r == R11)) gm |= 1u << i;
    }
    n = __builtin_popcount(gm) + __builtin_popcount(used_xmm_);
    bytes = ((n * 8 + 15) / 16) * 16;
    if (bytes) a_.sub_imm(RSP, bytes);
    int k = 0;
    for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) if (gm >> i & 1) a_.mov_store(mem(RSP, 8 * k++), kGprPool[i]);
    for (int r = 0; r < 16; r++) if (used_xmm_ >> r & 1) a_.movsd_store(mem(RSP, 8 * k++), static_cast<Xmm>(r));
}

inline void Emitter::restore_live(int32_t bytes) {
    uint32_t gm = 0;
    for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) {
        const Reg r = kGprPool[i];
        if ((used_gpr_ >> i & 1) && (r == RSI || r == RDI || r == R8 || r == R9 || r == R10 || r == R11)) gm |= 1u << i;
    }
    int k = 0;
    for (size_t i = 0; i < sizeof(kGprPool) / sizeof(kGprPool[0]); i++) if (gm >> i & 1) a_.mov_load(kGprPool[i], mem(RSP, 8 * k++));
    for (int r = 0; r < 16; r++) if (used_xmm_ >> r & 1) a_.movsd_load(static_cast<Xmm>(r), mem(RSP, 8 * k++));
    if (bytes) a_.add_imm(RSP, bytes);
}

inline void Emitter::emit_phi_moves(int32_t from, int32_t to) {
    const Block& S = g_.b[to];
    size_t idx = 0;
    for (; idx < S.preds.size(); idx++) if (S.preds[idx] == from) break;
    struct Move { Loc dst; int32_t src; Loc src_loc; bool from_scratch = false; };
    std::vector<Move> moves;
    for (int32_t id : S.nodes) {
        if (g_.n[id].op != K::Phi) break;
        if (g_.n[id].dead || idx >= g_.n[id].ins.size()) continue;
        const int32_t src = g_.n[id].ins[idx];
        const Loc& d = loc_[id];
        if (d.kind == kNone) continue;
        const Loc& s = loc_[src];
        if (s.kind != kNone && s.kind == d.kind && s.reg == d.reg && s.slot == d.slot) continue;   // same place
        moves.push_back({d, src, s, false});
    }
    auto same = [](const Loc& x, const Loc& y) { return x.kind == y.kind && x.reg == y.reg && x.slot == y.slot; };
    auto emit_move = [&](const Move& m) {
        const Node& sn = g_.n[m.src];
        const bool fp = g_.n[m.src].rep == Rep::F64 || m.dst.kind == kXmm;
        if (m.from_scratch) {
            if (m.dst.kind == kGpr) a_.mov(static_cast<Reg>(m.dst.reg), RAX);
            else if (m.dst.kind == kXmm) a_.movapd(static_cast<Xmm>(m.dst.reg), XMM15);
            else a_.mov_store(slot_mem(m.dst), fp ? RAX : RAX);
            return;
        }
        if (m.dst.kind == kXmm) {
            if (sn.op == K::ConstF64) { a_.mov_imm64(RDX, sn.imm); a_.movq_to_xmm(static_cast<Xmm>(m.dst.reg), RDX); }
            else if (m.src_loc.kind == kXmm) a_.movapd(static_cast<Xmm>(m.dst.reg), static_cast<Xmm>(m.src_loc.reg));
            else a_.movsd_load(static_cast<Xmm>(m.dst.reg), slot_mem(m.src_loc));
        } else if (m.dst.kind == kGpr) {
            const Reg d = static_cast<Reg>(m.dst.reg);
            if (sn.op == K::ConstTagged || sn.op == K::ConstBool || sn.op == K::ConstI32) a_.mov_imm64(d, sn.imm);
            else if (m.src_loc.kind == kGpr) a_.mov(d, static_cast<Reg>(m.src_loc.reg));
            else a_.mov_load(d, slot_mem(m.src_loc));
        } else {   // stack destination
            Reg r;
            if (sn.op == K::ConstTagged || sn.op == K::ConstBool || sn.op == K::ConstI32) { a_.mov_imm64(RCX, sn.imm); r = RCX; }
            else if (sn.op == K::ConstF64) { a_.mov_imm64(RCX, sn.imm); r = RCX; }
            else if (m.src_loc.kind == kGpr) r = static_cast<Reg>(m.src_loc.reg);
            else if (m.src_loc.kind == kXmm) { a_.movq_from_xmm(RCX, static_cast<Xmm>(m.src_loc.reg)); r = RCX; }
            else { a_.mov_load(RCX, slot_mem(m.src_loc)); r = RCX; }
            a_.mov_store(slot_mem(m.dst), r);
        }
    };
    while (!moves.empty()) {
        bool progress = false;
        for (size_t i = 0; i < moves.size(); i++) {
            bool blocked = false;
            for (size_t j = 0; j < moves.size(); j++)
                if (j != i && !moves[j].from_scratch && moves[j].src_loc.kind != kNone && same(moves[j].src_loc, moves[i].dst)) { blocked = true; break; }
            if (blocked) continue;
            emit_move(moves[i]);
            moves.erase(moves.begin() + i);
            progress = true;
            break;
        }
        if (progress) continue;
        // Only cycles are left: park one destination in a scratch register and let its readers use that.
        const Loc parked = moves[0].dst;
        if (parked.kind == kGpr) a_.mov(RAX, static_cast<Reg>(parked.reg));
        else if (parked.kind == kXmm) a_.movapd(XMM15, static_cast<Xmm>(parked.reg));
        else a_.mov_load(RAX, slot_mem(parked));
        for (Move& m : moves)
            if (!m.from_scratch && m.src_loc.kind != kNone && same(m.src_loc, parked)) { m.from_scratch = true; m.src_loc = Loc{}; }
    }
}

inline void Emitter::emit_node(int32_t id) {
    const Node& n = g_.n[id];
    cur_kind_ = kname(n.op);
    if (n.state >= 0 && config().stress) stress_guard(n.state);
    switch (n.op) {
    case K::ConstTagged: case K::ConstF64: case K::ConstBool: case K::ConstI32: case K::Phi:
        return;
    case K::LoadReg: {
        const Reg d = def_g(id, RAX);
        a_.mov_load(d, mem(R12, static_cast<int32_t>(n.aux * sizeof(Value))));
        commit_g(id, d);
        return;
    }
    case K::LoadAcc: {
        const Reg d = def_g(id, RAX);
        a_.mov_load(d, mem(RBX, static_cast<int32_t>(kOffAcc)));
        commit_g(id, d);
        return;
    }
    case K::CheckNum: {
        const Reg s = use_g(n.a, RAX);
        const Xmm d = def_x(id, XMM0);
        Label& cold = new_label();
        Label& back = new_label();
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 52);
        a_.cmp32_imm(RCX, 0x7FF);
        a_.jcc(CC_E, cold);
        a_.movq_to_xmm(d, s);
        a_.bind(back);
        commit_x(id, d);
        Label* dl = &deopt_label(n.state);
        cold_.push_back([this, s, d, &cold, &back, dl]() {
            Label& nan = new_label();
            Label& ninf = new_label();
            Label& pinf = new_label();
            a_.bind(cold);
            a_.mov(RCX, s);
            a_.shr_imm(RCX, 48);
            a_.cmp32_imm(RCX, 0x7FF8);
            a_.jcc(CC_E, nan);
            a_.cmp32_imm(RCX, 0x7FFA);
            a_.jcc(CC_E, ninf);
            a_.cmp32_imm(RCX, 0x7FFB);
            a_.jcc(CC_E, pinf);
            a_.jmp(*dl);
            a_.bind(nan);
            a_.mov_imm64(RDX, 0x7FF8000000000000ull);
            a_.movq_to_xmm(d, RDX);
            a_.jmp(back);
            a_.bind(ninf);
            a_.mov_imm64(RDX, 0xFFF0000000000000ull);
            a_.movq_to_xmm(d, RDX);
            a_.jmp(back);
            a_.bind(pinf);
            a_.mov_imm64(RDX, 0x7FF0000000000000ull);
            a_.movq_to_xmm(d, RDX);
            a_.jmp(back);
        });
        return;
    }
    case K::BoxF64: {
        const Xmm s = use_x(n.a, XMM0);
        const Reg d = def_g(id, RAX);
        emit_box_inline(d, s);
        commit_g(id, d);
        return;
    }
    case K::BoxBool: {
        const Reg b = use_g(n.a, RDX);
        const Reg d = def_g(id, RAX);
        a_.mov(RCX, b);
        a_.shl_imm(RCX, 48);
        a_.mov_imm64(d, 0x7FF2000000000000ull);
        a_.add(d, RCX);
        commit_g(id, d);
        return;
    }
    case K::ToI32: {
        const Xmm s = use_x(n.a, XMM0);
        const Reg d = def_g(id, RAX);
        Label& cold = new_label();
        Label& back = new_label();
        a_.cvttsd2si64(RCX, s);
        a_.mov_imm64(RDX, 0x8000000000000000ull);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_E, cold);
#ifdef QUANTA_JIT_ARCH_A64
        // fcvtzs saturates instead of answering one indefinite value: both ends go to the cold path.
        a_.mov_imm64(RDX, 0x7FFFFFFFFFFFFFFFull);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_E, cold);
#endif
        a_.mov32(d, RCX);
        a_.bind(back);
        commit_g(id, d);
        Label* dl = &deopt_label(n.state);
        cold_.push_back([this, s, d, &cold, &back, dl]() {
            Label& zero = new_label();
            a_.bind(cold);
            a_.movapd(XMM1, s);
            a_.subsd(XMM1, s);          // NaN for NaN and the infinities, 0 for the finite out-of-range
            a_.ucomisd(XMM1, XMM1);
            a_.jcc(CC_P, zero);
            a_.jmp(*dl);
            a_.bind(zero);
            a_.xor32(d, d);
            a_.jmp(back);
        });
        return;
    }
    case K::I32ToF64: {
        const Reg s = use_g(n.a, RAX);
        const Xmm d = def_x(id, XMM0);
        a_.cvtsi2sd32(d, s);
        commit_x(id, d);
        return;
    }
    case K::U32ToF64: {
        const Reg s = use_g(n.a, RAX);
        const Xmm d = def_x(id, XMM0);
        a_.mov32(RCX, s);
        a_.cvtsi2sd64(d, RCX);
        commit_x(id, d);
        return;
    }
    case K::AddF: case K::SubF: case K::MulF: case K::DivF: {
        const Xmm a = use_x(n.a, XMM0);
        const Xmm b = use_x(n.b, XMM1);
        Xmm d = def_x(id, XMM0);
        if (d == b && d != a) d = XMM0;     // the destination would be overwritten before b is read
        if (d != a) a_.movapd(d, a);
        switch (n.op) {
        case K::AddF: a_.addsd(d, b); break;
        case K::SubF: a_.subsd(d, b); break;
        case K::MulF: a_.mulsd(d, b); break;
        default: a_.divsd(d, b); break;
        }
        if (loc_[id].kind == kXmm && static_cast<Xmm>(loc_[id].reg) != d) a_.movapd(static_cast<Xmm>(loc_[id].reg), d);
        else commit_x(id, d);
        return;
    }
    case K::NegF: {
        const Xmm a = use_x(n.a, XMM0);
        const Xmm d = def_x(id, XMM0);
#ifdef QUANTA_JIT_ARCH_A64
        a_.fneg(d, a);
#else
        a_.mov_imm64(RDX, 0x8000000000000000ull);
        a_.movq_to_xmm(XMM1, RDX);
        if (d != a) a_.movapd(d, a);
        a_.xorpd(d, XMM1);
#endif
        commit_x(id, d);
        return;
    }
    case K::AndI: case K::OrI: case K::XorI: {
        const Reg a = use_g(n.a, RAX);
        const Reg b = use_g(n.b, RCX);
        Reg d = def_g(id, RAX);
        if (d == b && d != a) d = RAX;
        if (d != a) a_.mov32(d, a);
        if (n.op == K::AndI) a_.and32(d, b); else if (n.op == K::OrI) a_.or32(d, b); else a_.xor32(d, b);
        if (loc_[id].kind == kGpr && static_cast<Reg>(loc_[id].reg) != d) a_.mov32(static_cast<Reg>(loc_[id].reg), d);
        else commit_g(id, d);
        return;
    }
    case K::ShlI: case K::SarI: case K::ShrI: {
        const Reg a = use_g(n.a, RAX);
        const Reg b = use_g(n.b, RCX);
        if (b != RCX) a_.mov32(RCX, b);
        a_.mov32(RAX, a);
        if (n.op == K::ShlI) a_.shl32_cl(RAX); else if (n.op == K::SarI) a_.sar32_cl(RAX); else a_.shr32_cl(RAX);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::NotI: {
        const Reg a = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        if (d != a) a_.mov32(d, a);
        a_.not32(d);
        commit_g(id, d);
        return;
    }
    case K::LtF: case K::GtF: case K::LeF: case K::GeF: case K::EqF: case K::NeF: {
        const Xmm a = use_x(n.a, XMM0);
        const Xmm b = use_x(n.b, XMM1);
        const Reg d = def_g(id, RAX);
        a_.xor32(RAX, RAX);
        switch (n.op) {
#ifdef QUANTA_JIT_ARCH_A64
        // fcmp leaves "unordered" as C=1, V=1: the conditions below are the ones that are false for NaN.
        case K::LtF: a_.ucomisd(a, b); a_.setcc(CC_S, RAX); break;      // MI
        case K::GtF: a_.ucomisd(a, b); a_.setcc(CC_G, RAX); break;      // GT
        case K::LeF: a_.ucomisd(a, b); a_.setcc(CC_BE, RAX); break;     // LS
        case K::GeF: a_.ucomisd(a, b); a_.setcc(CC_GE, RAX); break;     // GE
#else
        case K::LtF: a_.ucomisd(b, a); a_.setcc(CC_A, RAX); break;
        case K::GtF: a_.ucomisd(a, b); a_.setcc(CC_A, RAX); break;
        case K::LeF: a_.ucomisd(b, a); a_.setcc(CC_AE, RAX); break;
        case K::GeF: a_.ucomisd(a, b); a_.setcc(CC_AE, RAX); break;
#endif
        case K::EqF:
            a_.xor32(RCX, RCX);
            a_.ucomisd(a, b);
            a_.setcc(CC_E, RAX);
            a_.setcc(CC_NP, RCX);
            a_.and32(RAX, RCX);
            break;
        default:   // NeF: not equal, or unordered
            a_.xor32(RCX, RCX);
            a_.ucomisd(a, b);
            a_.setcc(CC_NE, RAX);
            a_.setcc(CC_P, RCX);
            a_.or32(RAX, RCX);
            break;
        }
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::EqTag: case K::NeTag: {
        const Reg a = use_g(n.a, RAX);
        const Reg b = use_g(n.b, RCX);
        const Reg d = def_g(id, RDX);
        a_.cmp(a, b);
        a_.mov_imm32(RAX, 0);
        a_.setcc(n.op == K::EqTag ? CC_E : CC_NE, RAX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::Trap:
        a_.jmp(deopt_label(n.state));
        return;
    case K::IsFn: {
        const Reg s = use_g(n.a, RAX);
        Label& no = new_label();
        Label& done = new_label();
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 48);
        a_.cmp32_imm(RCX, 0x7FF9);                 // QUIET_NAN | TAG_FUNCTION
        a_.jcc(CC_NE, no);
        a_.mov(RCX, s);
        a_.shl_imm(RCX, 16);
        a_.shr_imm(RCX, 16);
        if (n.aux == 0) a_.mov_load(RCX, mem(RCX, static_cast<int32_t>(JitObjectLayout::function_executable())));
        a_.mov_imm64(RDX, n.imm);
        a_.cmp(RCX, RDX);
        a_.mov_imm32(RAX, 0);
        a_.setcc(CC_E, RAX);
        a_.jmp(done);
        a_.bind(no);
        a_.mov_imm32(RAX, 0);
        a_.bind(done);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::CheckBits: {
        const Reg s = use_g(n.a, RAX);
        a_.mov_imm64(RCX, n.imm);
        a_.cmp(s, RCX);
        a_.jcc(CC_NE, deopt_label(n.state));
        return;
    }
    case K::MathF: {
        const Xmm a = use_x(n.a, XMM0);
        const Xmm b = n.b >= 0 ? use_x(n.b, XMM1) : XMM1;
        const Xmm d = def_x(id, XMM0);
        if (n.b >= 0 && b != XMM1) a_.movapd(XMM1, b);
        if (a != XMM0) a_.movapd(XMM0, a);
        int32_t bytes;
        save_live(bytes);
        call_c(kMathFns[n.aux]);
        restore_live(bytes);
        if (d != XMM0) a_.movapd(d, XMM0);
        commit_x(id, d);
        return;
    }
    case K::CallNative: {
        Label& dl = deopt_label(n.state);
        // Laid out in the frame (the spill slots are addressed from rsp, so rsp must not move before they are read):
        // [0] argument 0, [8] argument 1, [16] the receiver.
        const int32_t area = native_args_ * 8;
        const uint64_t undefined_bits = std::bit_cast<uint64_t>(Value());
        for (int k = 0; k < 3; k++) {
            const int32_t operand = k == 2 ? n.a : k == 0 ? n.b : n.c;
            if (operand >= 0) {
                const Reg r = use_g(operand, RAX);
                a_.mov_store(mem(RSP, area + k * 8), r);
            } else {
                a_.mov_imm64(RAX, undefined_bits);
                a_.mov_store(mem(RSP, area + k * 8), RAX);
            }
        }
        int32_t bytes;
        save_live(bytes);
        a_.mov(abi::A0, RBX);
        a_.mov_imm64(abi::A1, n.imm);
        a_.mov_load(abi::A2, mem(RSP, bytes + area + 16));
        a_.lea(abi::A3, mem(RSP, bytes + area));
        call_c(reinterpret_cast<const void*>(&helper_call_native));
        restore_live(bytes);
        a_.cmp_imm(RAX, -1);
        a_.jcc(CC_E, dl);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::NewLiteral: {
        Label& dl = deopt_label(n.state);
        int32_t bytes;
        save_live(bytes);
        a_.mov_imm64(abi::A0, n.imm);
        a_.mov_load(abi::A1, mem(RBX, static_cast<int32_t>(kOffCtx)));
        call_c(reinterpret_cast<const void*>(&helper_new_literal));
        restore_live(bytes);
        a_.test(RAX, RAX);
        a_.jcc(CC_E, dl);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::CheckNotNullish: {
        const Reg s = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 48);
        a_.sub32_imm(RCX, 0x7FF0);
        a_.cmp32_imm(RCX, 1);
        a_.jcc(CC_BE, deopt_label(n.state));
        if (d != s) a_.mov(d, s);
        commit_g(id, d);
        return;
    }
    case K::NewObject: {
        Label& dl = deopt_label(n.state);
        const Reg fr = use_g(n.a, RAX);
        a_.mov_imm64(RCX, n.imm);
        a_.push(fr);
        a_.push(RCX);
        int32_t bytes;
        save_live(bytes);
        a_.mov_load(abi::A1, mem(RSP, bytes));
        a_.mov_load(abi::A0, mem(RSP, bytes + kPushBytes));
        call_c(reinterpret_cast<const void*>(&helper_new_object));
        restore_live(bytes);
        a_.pop(RCX);
        a_.pop(RCX);
        a_.test(RAX, RAX);
        a_.jcc(CC_E, dl);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::LoadPoly: {
        const Reg o = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        const Graph::Poly& p = g_.polys[n.aux];
        Label& dl = deopt_label(n.state);
        Label& done = new_label();
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::shape())));
        // An object with a descriptor map of its own may override the slot: not served here, as for CheckShape.
        a_.test8_mem_imm(mem(o, static_cast<int32_t>(JitObjectLayout::proto())), static_cast<uint8_t>(JitObjectLayout::kHasDescriptors));
        a_.jcc(CC_NE, dl);
        for (uint8_t k = 0; k < p.n; k++) {
            Label& miss = new_label();
            a_.mov_imm64(RDX, p.word[k]);
            a_.cmp(RCX, RDX);
            a_.jcc(CC_NE, miss);
            a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
            a_.mov_load(d, mem(RCX, static_cast<int32_t>(p.slot[k] * sizeof(Value))));
            a_.jmp(done);
            a_.bind(miss);
        }
        a_.jmp(dl);
        a_.bind(done);
        commit_g(id, d);
        return;
    }
    case K::IsNullish: {
        const Reg a = use_g(n.a, RAX);
        const Reg d = def_g(id, RDX);
        a_.mov(RCX, a);
        a_.shr_imm(RCX, 48);
        a_.sub32_imm(RCX, 0x7FF0);            // undefined is 0x7FF0, null 0x7FF1
        a_.cmp32_imm(RCX, 1);
        a_.mov_imm32(RAX, 0);
        a_.setcc(CC_BE, RAX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::NotBool: {
        const Reg a = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        a_.mov_imm32(RCX, 1);
        if (d != a) a_.mov32(d, a);
        a_.xor32(d, RCX);
        commit_g(id, d);
        return;
    }
    case K::TruthyF: {
        const Xmm a = use_x(n.a, XMM0);
        const Reg d = def_g(id, RAX);
        a_.xor32(RAX, RAX);
        a_.xor32(RCX, RCX);
        a_.xorpd(XMM1, XMM1);
        a_.ucomisd(a, XMM1);
        a_.setcc(CC_NE, RAX);      // not zero ...
        a_.setcc(CC_NP, RCX);      // ... and not NaN
        a_.and32(RAX, RCX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::Truthy: {
        const Reg s = use_g(n.a, RDX);
        const Reg d = def_g(id, RAX);
        Label& generic = new_label();
        Label& done = new_label();
        Label& is_true = new_label();
        Label& is_false = new_label();
        Label& finite = new_label();
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 52);
        a_.cmp32_imm(RCX, 0x7FF);
        a_.jcc(CC_NE, finite);
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 48);
        a_.cmp32_imm(RCX, 0x7FF3); a_.jcc(CC_E, is_true);
        a_.cmp32_imm(RCX, 0x7FF2); a_.jcc(CC_E, is_false);
        a_.cmp32_imm(RCX, 0x7FF0); a_.jcc(CC_E, is_false);
        a_.cmp32_imm(RCX, 0x7FF1); a_.jcc(CC_E, is_false);
        a_.cmp32_imm(RCX, 0x7FF7); a_.jcc(CC_E, is_true);
        a_.cmp32_imm(RCX, 0x7FF9); a_.jcc(CC_E, is_true);
        a_.jmp(generic);
        a_.bind(finite);
        a_.mov(RCX, s);
        a_.add(RCX, RCX);
        a_.xor32(RAX, RAX);
        a_.test(RCX, RCX);
        a_.setcc(CC_NE, RAX);
        a_.jmp(done);
        a_.bind(is_true);
        a_.mov_imm32(RAX, 1);
        a_.jmp(done);
        a_.bind(is_false);
        a_.xor32(RAX, RAX);
        a_.bind(done);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        cold_.push_back([this, s, d, &generic, &done]() {
            a_.bind(generic);
            int32_t bytes;
            save_live(bytes);
            if (s != abi::A0) a_.mov(abi::A0, s);
            call_c(reinterpret_cast<const void*>(&helper_truthy));
            restore_live(bytes);
            a_.jmp(done);
        });
        return;
    }
    case K::NotTdz: {
        const Reg s = use_g(n.a, RAX);
        a_.mov_imm64(RCX, kTdzBits);
        a_.cmp(s, RCX);
        a_.jcc(CC_E, deopt_label(n.state));
        const Reg d = def_g(id, RAX);
        if (d != s) a_.mov(d, s);
        commit_g(id, d);
        return;
    }
    case K::StoreReg: {
        const Reg s = use_g(n.a, RAX);
        a_.mov_store(mem(R12, static_cast<int32_t>(n.aux * sizeof(Value))), s);
        return;
    }
    case K::StoreAcc: {
        const Reg s = use_g(n.a, RAX);
        a_.mov_store(mem(RBX, static_cast<int32_t>(kOffAcc)), s);
        return;
    }
    case K::Step: {
        const uint8_t op = g_.chunk->code[n.aux];
        int32_t bytes = 0;
        if (n.light) save_live(bytes);              // the values that live across it
        a_.mov(abi::A0, RBX);
        a_.mov_imm32(abi::A1, n.aux);
        a_.mov_imm32(abi::A2, static_cast<uint32_t>(n.imm));
        a_.mov_imm32(abi::A3, kNoOperand);
        call_c(reinterpret_cast<const void*>(kStepFns[op]));
        if (n.light) restore_live(bytes);
        const Reg d = def_g(id, RAX);
        if (d != RAX) a_.mov32(d, RAX);
        commit_g(id, d);
        return;
    }
    case K::StepExit: {
        const Reg s = use_g(n.a, RAX);
        if (n.imm == kNoPc) {
            a_.mov32(RAX, s);
            a_.jmp(step_exit_);
            return;
        }
        Label& cold = new_label();
        a_.cmp32_imm(s, static_cast<int32_t>(n.imm));
        a_.jcc(CC_NE, cold);
        cold_.push_back([this, s, &cold]() {
            a_.bind(cold);
            a_.mov32(RAX, s);
            a_.jmp(step_exit_);
        });
        return;
    }
    case K::Poll: {
        Label& cold = new_label();
        Label& back = new_label();
        a_.sub32_mem_imm(mem(R14, static_cast<int32_t>(offsetof(JitRt, sp_counter))), 1);
        a_.jcc(CC_LE, cold);
        a_.bind(back);
        cold_.push_back([this, &cold, &back]() {
            a_.bind(cold);
            int32_t bytes;
            save_live(bytes);
            a_.mov(abi::A0, R14);
            call_c(reinterpret_cast<const void*>(&helper_poll));
            restore_live(bytes);
            a_.jmp(back);
        });
        return;
    }
    case K::LoadThis: {
        const Reg d = def_g(id, RAX);
        Label& dl = deopt_label(n.state);
        a_.cmp8_mem_imm(mem(RBX, static_cast<int32_t>(kOffThisResolved)), 0);
        a_.jcc(CC_E, dl);
        a_.mov_load(RCX, mem(RBX, static_cast<int32_t>(kOffCtx)));
        a_.test8_mem_imm(mem(RCX, static_cast<int32_t>(g_cfg.this_needs_super_off)), g_cfg.this_needs_super_mask);
        a_.jcc(CC_NE, dl);
        a_.mov_load(d, mem(RBX, static_cast<int32_t>(kOffThisValue)));
        commit_g(id, d);
        return;
    }
    case K::LoadEnv: case K::StoreEnv: {
        const bool store = n.op == K::StoreEnv;
        Label& dl = deopt_label(n.state);
        Label& via_ctx = new_label();
        Label& have = new_label();
        Label& slow = store ? dl : new_label();   // a load that misses its slot asks the runtime to find the binding
        Label& done = new_label();
        const Reg v = store ? use_g(n.a, RDX) : RDX;
        a_.cmp_mem_imm(mem(RBX, static_cast<int32_t>(kOffCallInfo)), 0);
        a_.jcc(CC_E, via_ctx);
        a_.mov_load(RAX, mem(RBX, static_cast<int32_t>(kOffCurLexEnv)));
        a_.jmp(have);
        a_.bind(via_ctx);
        a_.mov_load(RAX, mem(RBX, static_cast<int32_t>(kOffCtx)));
        a_.mov_load(RAX, mem(RAX, static_cast<int32_t>(JitEnvLayout::ctx_lexical_environment())));
        a_.bind(have);
        a_.test(RAX, RAX);
        a_.jcc(CC_E, slow);
        for (uint32_t h = n.aux >> 8; h > 0; h--) {      // the enclosing scopes, for a variable of an outer function
            a_.mov_load(RAX, mem(RAX, static_cast<int32_t>(JitEnvLayout::outer())));
            a_.test(RAX, RAX);
            a_.jcc(CC_E, slow);
        }
        const uint32_t slot_index = n.aux & 0xFF;
        // The entry: one of the inline ones, or one in the exact-sized array behind them.
        Reg base = RAX;
        int32_t disp;
        if (slot_index < JitEnvLayout::kInlineSlots) {
            disp = static_cast<int32_t>(JitEnvLayout::entry(slot_index));
        } else {
            const uint32_t e = slot_index - JitEnvLayout::kInlineSlots;
            a_.mov_load32(RCX, mem(RAX, static_cast<int32_t>(JitEnvLayout::ext_capacity())));
            a_.and32_imm(RCX, 0xFFFF);
            a_.cmp32_imm(RCX, static_cast<int32_t>(e));
            a_.jcc(CC_BE, slow);
            a_.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitEnvLayout::ext())));
            base = RCX;
            disp = static_cast<int32_t>(e * JitEnvLayout::kEntrySize);
        }
        a_.mov_imm64(R11, n.imm);
        a_.cmp_load(R11, mem(base, disp + static_cast<int32_t>(JitEnvLayout::key())));
        a_.jcc(CC_NE, slow);
        a_.cmp8_mem_imm(mem(base, disp + static_cast<int32_t>(JitEnvLayout::initialized())), 0);
        a_.jcc(CC_E, slow);
        if (store) {
            a_.cmp8_mem_imm(mem(base, disp + static_cast<int32_t>(JitEnvLayout::mutable_flag())), 0);
            a_.jcc(CC_E, dl);
            a_.mov_store(mem(base, disp + static_cast<int32_t>(JitEnvLayout::value())), v);
            emit_env_barrier_check(n.a, v);
        } else {
            const Reg d = def_g(id, RDX);
            a_.mov_load(d, mem(base, disp + static_cast<int32_t>(JitEnvLayout::value())));
            a_.bind(done);
            commit_g(id, d);
            const uint64_t key = n.imm;
            const Reg dd = d;
            cold_.push_back([this, &slow, &done, dl_ = &dl, key, dd]() {
                a_.bind(slow);
                int32_t bytes;
                save_live(bytes);
                a_.sub_imm(RSP, 16);
                a_.mov(abi::A0, RBX);
                a_.mov_imm64(abi::A1, key);
                a_.mov(abi::A2, RSP);
                call_c(reinterpret_cast<const void*>(&helper_env_load));
                a_.mov_load(RDX, mem(RSP, 0));
                a_.add_imm(RSP, 16);
                a_.test32(RAX, RAX);       // (after the add: it sets the flags)
                Label& fail = new_label();
                a_.jcc(CC_E, fail);
                restore_live(bytes);
                a_.mov(dd, RDX);
                a_.jmp(done);
                a_.bind(fail);
                restore_live(bytes);
                a_.jmp(*dl_);
            });
        }
        return;
    }
    case K::CheckFn: {
        const Reg s = use_g(n.a, RAX);
        Label& dl = deopt_label(n.state);
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 48);
        a_.cmp32_imm(RCX, 0x7FF9);                 // QUIET_NAN | TAG_FUNCTION
        a_.jcc(CC_NE, dl);
        a_.mov(RCX, s);
        a_.shl_imm(RCX, 16);
        a_.shr_imm(RCX, 16);
        if (n.aux == 0) a_.mov_load(RCX, mem(RCX, static_cast<int32_t>(JitObjectLayout::function_executable())));
        a_.mov_imm64(RDX, n.imm);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_NE, dl);
        return;
    }
    case K::LoadLookup: {
        const Reg d = def_g(id, RAX);
        Label& dl = deopt_label(n.state);
        if (n.aux & 0x100) {
            a_.mov_load(RCX, mem(RBX, static_cast<int32_t>(kOffLookup)));
            if (n.imm) a_.add_imm(RCX, static_cast<int32_t>(n.imm));
        } else {
            a_.mov_imm64(RCX, n.imm);
        }
        a_.mov_imm64(RDX, reinterpret_cast<uint64_t>(JitEnvLayout::shadow_epoch_addr()));
        a_.mov_load32(RDX, mem(RDX));
        a_.cmp32_load(RDX, mem(RCX, static_cast<int32_t>(JitLookupLayout::shadow_epoch())));
        a_.jcc(CC_NE, dl);
        if ((n.aux & 3) == 0) {
            a_.mov_load(RDX, mem(RCX, static_cast<int32_t>(JitLookupLayout::slot())));
            a_.test(RDX, RDX);
            a_.jcc(CC_E, dl);
            a_.cmp_mem_imm(mem(RCX, static_cast<int32_t>(JitLookupLayout::obj_shape())), 0);
            a_.jcc(CC_NE, dl);
            a_.mov_load(d, mem(RDX));
        } else if ((n.aux & 3) == 2) {
            // A global in dictionary mode: its value sits in a descriptor of the binding object.
            a_.mov_load(RDX, mem(RCX, static_cast<int32_t>(JitLookupLayout::dict_desc())));
            a_.test(RDX, RDX);
            a_.jcc(CC_E, dl);
            a_.mov_load(R11, mem(RCX, static_cast<int32_t>(JitLookupLayout::env())));
            a_.mov_load(R11, mem(R11, static_cast<int32_t>(JitEnvLayout::binding_object())));
            a_.test(R11, R11);
            a_.jcc(CC_E, dl);
            a_.mov_load(RAX, mem(R11, static_cast<int32_t>(JitObjectLayout::shape())));
            a_.shr_imm(RAX, 5);
            a_.shl_imm(RAX, 5);
            a_.test(RAX, RAX);
            a_.jcc(CC_NE, dl);                       // it must still have no shape
            a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(JitObjectLayout::descriptor_epoch_addr()));
            a_.mov_load(RAX, mem(RAX));
            a_.cmp_load(RAX, mem(RCX, static_cast<int32_t>(JitLookupLayout::descriptor_epoch())));
            a_.jcc(CC_NE, dl);
            a_.cmp32_mem_imm(mem(RDX, static_cast<int32_t>(JitObjectLayout::descriptor_type())), 0);
            a_.jcc(CC_NE, dl);
            a_.mov_load(d, mem(RDX, static_cast<int32_t>(JitObjectLayout::descriptor_value())));
        } else {
            a_.mov_load(RDX, mem(RCX, static_cast<int32_t>(JitLookupLayout::obj_shape())));
            a_.test(RDX, RDX);
            a_.jcc(CC_E, dl);
            a_.mov_load(R11, mem(RCX, static_cast<int32_t>(JitLookupLayout::env())));
            a_.mov_load(R11, mem(R11, static_cast<int32_t>(JitEnvLayout::binding_object())));
            a_.test(R11, R11);
            a_.jcc(CC_E, dl);
            a_.mov_load(RAX, mem(R11, static_cast<int32_t>(JitObjectLayout::shape())));
            a_.shr_imm(RAX, 5);
            a_.shl_imm(RAX, 5);
            a_.cmp(RAX, RDX);
            a_.jcc(CC_NE, dl);
            a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(JitObjectLayout::descriptor_epoch_addr()));
            a_.mov_load(RAX, mem(RAX));
            a_.cmp_load(RAX, mem(RCX, static_cast<int32_t>(JitLookupLayout::descriptor_epoch())));
            a_.jcc(CC_NE, dl);
            a_.mov_load32(RDX, mem(RCX, static_cast<int32_t>(JitLookupLayout::obj_slot_index())));
            a_.mov_load(R11, mem(R11, static_cast<int32_t>(JitObjectLayout::butterfly())));
            a_.mov_load(d, mem_idx(R11, RDX, 8, 0));
        }
        commit_g(id, d);
        return;
    }
    case K::CheckProto: {
        const Reg o = use_g(n.a, RAX);
        Label& dl = deopt_label(n.state);
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::proto())));
        a_.shr_imm(RCX, 4);
        a_.shl_imm(RCX, 4);
        a_.mov_imm64(RDX, n.imm);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_NE, dl);
        return;
    }
    case K::CheckEpoch: {
        Label& dl = deopt_label(n.state);
        a_.mov_imm64(RCX, reinterpret_cast<uint64_t>(n.aux ? JitObjectLayout::descriptor_epoch_addr() : JitObjectLayout::proto_epoch_addr()));
        a_.mov_load(RCX, mem(RCX));
        a_.mov_imm64(RDX, n.imm);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_NE, dl);
        return;
    }
    case K::LoadHolder: {
        const Reg d = def_g(id, RAX);
        a_.mov_imm64(RCX, n.imm);
        a_.mov_load(RCX, mem(RCX, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.mov_load(d, mem(RCX, static_cast<int32_t>(n.aux * sizeof(Value))));
        commit_g(id, d);
        return;
    }
    case K::CheckObj: {
        const Reg s = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        a_.mov(RCX, s);
        a_.shr_imm(RCX, 48);
        a_.cmp32_imm(RCX, 0x7FF7);
        a_.jcc(CC_NE, deopt_label(n.state));
        if (d != s) a_.mov(d, s);
        a_.shl_imm(d, 16);
        a_.shr_imm(d, 16);
        commit_g(id, d);
        return;
    }
    case K::CheckShape: {
        const Reg o = use_g(n.a, RAX);
        Label& dl = deopt_label(n.state);
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::shape())));
        a_.mov_imm64(RDX, n.imm);
        a_.cmp(RCX, RDX);
        a_.jcc(CC_NE, dl);
        a_.test8_mem_imm(mem(o, static_cast<int32_t>(JitObjectLayout::proto())), static_cast<uint8_t>(JitObjectLayout::kHasDescriptors));
        a_.jcc(CC_NE, dl);
        return;
    }
    case K::LoadSlot: {
        const Reg o = use_g(n.a, RAX);
        const Reg d = def_g(id, RAX);
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.mov_load(d, mem(RCX, static_cast<int32_t>(n.aux * sizeof(Value))));
        commit_g(id, d);
        return;
    }
    case K::StoreSlot: {
        const Reg o = use_g(n.a, RAX);
        const Reg v = use_g(n.b, RDX);
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.mov_store(mem(RCX, static_cast<int32_t>(n.aux * sizeof(Value))), v);
        emit_barrier_check(n.b, o, v);
        return;
    }
    case K::CheckDense: {
        const Reg o = use_g(n.a, RAX);
        Label& dl = deopt_label(n.state);
        a_.mov_load(RCX, mem(o, static_cast<int32_t>(JitObjectLayout::shape())));
        a_.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kShapeTypeMask));
        a_.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kArrayType));
        a_.jcc(CC_NE, dl);
        a_.mov_load(RDX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.test(RDX, RDX);
        a_.jcc(CC_E, dl);
        a_.mov_load32(RCX, mem(RDX, -static_cast<int32_t>(sizeof(Value))));
        a_.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a_.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a_.jcc(CC_NE, dl);
        return;
    }
    case K::NonNegI32: {
        const Reg s2 = use_g(n.a, RAX);
        a_.test32(s2, s2);
        a_.jcc(CC_S, deopt_label(n.state));
        const Reg d = def_g(id, RAX);
        if (d != s2) a_.mov32(d, s2);
        commit_g(id, d);
        return;
    }
    case K::ToIndex: {
        const Xmm x = use_x(n.a, XMM0);
        const Reg d = def_g(id, RAX);
        Label& dl = deopt_label(n.state);
        a_.cvttsd2si32(RCX, x);
        a_.cvtsi2sd32(XMM1, RCX);
        a_.ucomisd(x, XMM1);
        a_.jcc(CC_P, dl);
        a_.jcc(CC_NE, dl);
        a_.test32(RCX, RCX);
        a_.jcc(CC_S, dl);
        a_.mov32(d, RCX);
        commit_g(id, d);
        return;
    }
    case K::LoadElem: {
        const Reg o = use_g(n.a, RAX);
        const Reg i = use_g(n.b, RCX);
        const Reg d = def_g(id, RAX);
        a_.mov_load(RDX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.cmp32_load(i, mem(RDX, -static_cast<int32_t>(JitObjectLayout::kExtrasBack) + static_cast<int32_t>(JitObjectLayout::kElementsLengthOff)));
        a_.jcc(CC_AE, deopt_label(n.state));
        if (i != RCX) a_.mov32(RCX, i);
        a_.neg64(RCX);
        a_.mov_load(d, mem_idx(RDX, RCX, 8, -static_cast<int32_t>(JitObjectLayout::kElementBase * sizeof(Value))));
        commit_g(id, d);
        return;
    }
    case K::StoreElem: {
        const Reg o = use_g(n.a, RAX);
        const Reg i = use_g(n.b, RCX);
        const Reg v = use_g(n.c, RDX);
        a_.mov_load(R11, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.cmp32_load(i, mem(R11, -static_cast<int32_t>(JitObjectLayout::kExtrasBack) + static_cast<int32_t>(JitObjectLayout::kElementsLengthOff)));
        a_.jcc(CC_AE, deopt_label(n.state));
        if (i != RCX) a_.mov32(RCX, i);
        a_.neg64(RCX);
        a_.mov_store(mem_idx(R11, RCX, 8, -static_cast<int32_t>(JitObjectLayout::kElementBase * sizeof(Value))), v);
        emit_barrier_check(n.c, o, v);
        return;
    }
    case K::ModF: {
        const Xmm a = use_x(n.a, XMM0);
        const Xmm b = use_x(n.b, XMM1);
        const Xmm d = def_x(id, XMM0);
        Label& cold = new_label();
        Label& done = new_label();
        Label& have = new_label();
        // Both operands int32-valued, the divisor neither 0 nor -1: a hardware remainder.
        a_.cvttsd2si32(RAX, a);
        a_.cvtsi2sd32(XMM15, RAX);
        a_.ucomisd(a, XMM15);
        a_.jcc(CC_P, cold);
        a_.jcc(CC_NE, cold);
        a_.cvttsd2si32(RCX, b);
        a_.cvtsi2sd32(XMM15, RCX);
        a_.ucomisd(b, XMM15);
        a_.jcc(CC_P, cold);
        a_.jcc(CC_NE, cold);
        a_.test32(RCX, RCX);
        a_.jcc(CC_E, cold);
        a_.cmp32_imm(RCX, -1);
        a_.jcc(CC_E, cold);
        a_.mov32(R11, RAX);
        a_.rem32(RDX, RAX, RCX);
        a_.test32(RDX, RDX);
        a_.jcc(CC_NE, have);
        {   // a zero remainder takes the dividend's sign
            Label& negz = new_label();
            a_.test32(R11, R11);
            a_.jcc(CC_S, negz);
            a_.bind(have);
            a_.cvtsi2sd32(d, RDX);
            a_.jmp(done);
            a_.bind(negz);
            a_.mov_imm64(RDX, 0x8000000000000000ull);
            a_.movq_to_xmm(d, RDX);
        }
        a_.bind(done);
        commit_x(id, d);
        cold_.push_back([this, a, b, d, &cold, &done]() {
            a_.bind(cold);
            if (b != XMM1) a_.movapd(XMM1, b);
            if (a != XMM0) a_.movapd(XMM0, a);
            int32_t bytes;
            save_live(bytes);
            call_c(reinterpret_cast<const void*>(&helper_fmod));
            restore_live(bytes);
            if (d != XMM0) a_.movapd(d, XMM0);
            a_.jmp(done);
        });
        return;
    }
    case K::ArrayLen: {
        const Reg o = use_g(n.a, RAX);
        const Xmm d = def_x(id, XMM0);
        a_.mov_load(RDX, mem(o, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a_.mov_load32(RCX, mem(RDX, -static_cast<int32_t>(JitObjectLayout::kExtrasBack) + static_cast<int32_t>(JitObjectLayout::kElementsLengthOff)));
        a_.cvtsi2sd64(d, RCX);
        commit_x(id, d);
        return;
    }

    default:
        why_ = "unlowered node";
        return;
    }
}

inline void Emitter::emit_term(int32_t blk, size_t layout_index) {
    const Term& t = g_.b[blk].term;
    const int32_t next_blk = layout_index + 1 < order_.size() ? order_[layout_index + 1] : -1;
    auto goto_blk = [&](int32_t target) { if (target != next_blk) a_.jmp(*blk_label_[target]); };
    switch (t.kind) {
    case T::Jump:
        emit_phi_moves(blk, t.t);
        goto_blk(t.t);
        return;
    case T::Branch: {
        const Reg c = use_g(t.cond, RAX);
        a_.test32(c, c);
        if (t.t == next_blk) { a_.jcc(CC_E, *blk_label_[t.f]); }
        else { a_.jcc(CC_NE, *blk_label_[t.t]); goto_blk(t.f); }
        return;
    }
    case T::Return: {
        const Reg v = use_g(t.cond, RAX);
        a_.mov_store(mem(RBX, static_cast<int32_t>(kOffAcc)), v);
        a_.xor32(RAX, RAX);
        a_.jmp(epilogue_);
        return;
    }
    case T::StepBranch: {
        const Reg s = use_g(t.cond, RAX);
        a_.mov32(RAX, s);
        a_.cmp32_imm(RAX, static_cast<int32_t>(t.tpc));
        a_.jcc(CC_E, *blk_label_[t.t]);
        a_.cmp32_imm(RAX, static_cast<int32_t>(t.fpc));
        if (t.f == next_blk) a_.jcc(CC_NE, step_exit_);
        else { a_.jcc(CC_E, *blk_label_[t.f]); a_.jmp(step_exit_); }
        return;
    }
    case T::Exit:
        a_.jmp(step_exit_);
        return;
    case T::Bail:
        a_.mov_store32_imm(mem(RBX, static_cast<int32_t>(kOffPc)), t.tpc);
        a_.mov_imm32(RAX, 2);
        a_.jmp(epilogue_);
        return;
    default:
        why_ = "block without a terminator";
        return;
    }
}

inline bool Emitter::emit(std::vector<uint8_t>& out, std::vector<std::pair<uint32_t, uint32_t>>& entries) {
    if (!allocate()) return false;
    blk_label_.assign(g_.b.size(), nullptr);
    state_label_.assign(g_.states.size(), nullptr);
    for (int32_t blk : order_) blk_label_[blk] = &new_label();

    // Prologue: the entry address arrives in the third argument register.
#ifdef QUANTA_JIT_ARCH_A64
    a_.enter_frame();
#else
    a_.push(RBP); a_.push(RBX); a_.push(R12); a_.push(R13); a_.push(R14); a_.push(R15);
    if (abi::kPreservesRdiRsi) { a_.push(RDI); a_.push(RSI); }
#endif
    if (frame_bytes_) a_.sub_imm(RSP, frame_bytes_);
#ifdef QUANTA_JIT_WIN64
    for (int i = 0; i < 10; i++) a_.movups_store(mem(RSP, xmm_save_off_ + 16 * i), static_cast<Xmm>(6 + i));
#endif
    a_.mov(RBX, abi::A0);
    a_.mov(R14, abi::A1);
    a_.mov_load(R12, mem(RBX, static_cast<int32_t>(kOffRegs)));
    a_.jmp_reg(abi::A2);

    for (size_t i = 0; i < order_.size() && !why_; i++) {
        const int32_t blk = order_[i];
        a_.bind(*blk_label_[blk]);
        if (g_.b[blk].synthetic && g_.b[blk].osr_pc >= 0)
            entries.push_back({static_cast<uint32_t>(g_.b[blk].osr_pc), static_cast<uint32_t>(a_.size())});
        for (int32_t id : g_.b[blk].nodes) {
            if (g_.n[id].dead) continue;
            emit_node(id);
            if (why_) break;
        }
        if (why_) break;
        emit_term(blk, i);
    }
    if (why_) return false;

    // Shared tails.
    a_.bind(step_exit_);       // eax = a Step's result that was not the fall-through
    {
        Label& finished = new_label();
        Label& rethrow = new_label();
        Label& bail = new_label();
        a_.cmp32_imm(RAX, kStepFinished);
        a_.jcc(CC_E, finished);
        a_.cmp32_imm(RAX, kStepRethrow);
        a_.jcc(CC_E, rethrow);
        a_.cmp32_imm(RAX, kStepBail);
        a_.jcc(CC_E, bail);
        a_.mov_store32(mem(RBX, static_cast<int32_t>(kOffPc)), RAX);   // continue at that pc in the interpreter
        a_.bind(bail);
        a_.mov_imm32(RAX, 2);
        a_.jmp(epilogue_);
        a_.bind(finished);
        a_.xor32(RAX, RAX);
        a_.jmp(epilogue_);
        a_.bind(rethrow);
        a_.mov_imm32(RAX, 1);
        a_.jmp(epilogue_);
    }
    for (size_t i = 0; i < cold_.size(); i++) cold_[i]();   // may append more
    for (size_t s = 0; s < state_label_.size(); s++) {
        if (!state_label_[s]) continue;
        a_.bind(*state_label_[s]);
        emit_deopt_stub(static_cast<int32_t>(s));
    }
    if (why_) return false;
    a_.bind(epilogue_);
#ifdef QUANTA_JIT_WIN64
    for (int i = 0; i < 10; i++) a_.movups_load(static_cast<Xmm>(6 + i), mem(RSP, xmm_save_off_ + 16 * i));
#endif
    if (frame_bytes_) a_.add_imm(RSP, frame_bytes_);
#ifdef QUANTA_JIT_ARCH_A64
    a_.leave_frame();
    if (a_.failed()) return fail("branch out of range");
#else
    if (abi::kPreservesRdiRsi) { a_.pop(RSI); a_.pop(RDI); }
    a_.pop(R15); a_.pop(R14); a_.pop(R13); a_.pop(R12); a_.pop(RBX); a_.pop(RBP);
    a_.ret();
#endif
    out = std::move(a_.buf);
    return true;
}

#endif  // QUANTA_OPT_BACKEND

// ===========================================================================
// Configuration and entry points
// ===========================================================================

inline void dump_chunk(const BytecodeChunk& chunk, uint32_t pc) {
    static std::unordered_set<const BytecodeChunk*> seen;
    if (!seen.insert(&chunk).second) return;
    Graph g;
    g.chunk = &chunk;
    Builder b(g);
    if (!b.build({pc}, config().max_insns)) {
        std::fprintf(stderr, "[opt] chunk %p refused: %s\n", static_cast<const void*>(&chunk), g.why ? g.why : "?");
        return;
    }
    std::fprintf(stderr, "[opt] chunk %p (%u bytes of bytecode), offer at pc %u\n", static_cast<const void*>(&chunk),
                 static_cast<unsigned>(chunk.code.size()), pc);
    dump(g, stderr);
}


inline bool has_calls(const BytecodeChunk& chunk) {
    for (uint32_t pc = 0; pc < chunk.code.size();) {
        if (chunk.code[pc] >= static_cast<uint8_t>(Op::kCount)) return false;
        const Op op = static_cast<Op>(chunk.code[pc]);
        if (op == Op::Call || op == Op::CallResolved || op == Op::Construct) return true;
        pc += 1 + static_cast<uint32_t>(op_operand_bytes(op));
    }
    return false;
}

inline std::vector<JitRt*>& all_rts() { static std::vector<JitRt*> v; return v; }

inline void print_unit_stats() {
    if (!config().log) return;
    for (JitRt* rt : all_rts())
        std::fprintf(stderr, "[opt] unit pc %u: %llu runs, %llu exits, %llu cycles inside\n", rt->opt_pc, (unsigned long long)rt->opt_runs, (unsigned long long)rt->opt_exits, (unsigned long long)rt->opt_cycles);
}

// A JitCode a chunk gets for the optimizing tier when the baseline JIT has none (it was refused or is
// off): stepping needs a private copy of the bytecode with a stop byte behind it and a table of the
// places a step may stop at.
inline JitCode* ensure_jitcode(const BytecodeChunk& chunk) {
    if (chunk.jit) return chunk.jit.get();
    std::unique_ptr<JitCode, JitCodeDeleter> jc(new JitCode());
    const uint32_t size = static_cast<uint32_t>(chunk.code.size());
    jc->code_size = size;
    jc->code_copy.assign(chunk.code.data(), chunk.code.data() + size);
    jc->code_copy.push_back(kHaltOp);
    jc->native_off.assign(size, 0);
    for (uint32_t pc = 0; pc < size;) {
        jc->native_off[pc] = 1;
        pc += 1 + static_cast<uint32_t>(op_operand_bytes(static_cast<Op>(chunk.code[pc])));
    }
    jc->owner_thread = &g_thread_marker;
    chunk.jit = std::move(jc);
    return chunk.jit.get();
}

#ifdef QUANTA_OPT_BACKEND
// Builds, allocates and emits. On success the chunk's JitCode has its optimized entry points.
inline bool compile(const BytecodeChunk& chunk, uint32_t pc, const Frame* peek, JitCode::OptUnit& unit) {
    {
        const uint32_t ordinal = config().seen++;
        if (ordinal < config().skip || ordinal - config().skip >= config().limit) return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer { std::chrono::steady_clock::time_point t0; ~Timer() { if (config().log) std::fprintf(stderr, "[opt] compile took %.3f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); } } timer{t0};
    // A callee that turns out to need a step makes its call site not worth inlining, and the build starts over.
    std::set<Builder::Site> no_inline;
    auto gp = std::make_unique<Graph>();
    for (int attempt = 0;; attempt++) {
        gp = std::make_unique<Graph>();
        gp->chunk = &chunk;
        gp->frame = peek;
        gp->entry_pc = pc;
        gp->fn_entry = pc == 0;
        Builder b(*gp, &no_inline);
        if (b.build({pc}, config().max_insns)) break;
        if (gp->failed_site.first && attempt < 8) {
            if (config().log) std::fprintf(stderr, "[opt] not inlining the call at pc %u of chunk %p: %s\n", gp->failed_site.second, static_cast<const void*>(gp->failed_site.first), gp->why ? gp->why : "?");
            no_inline.insert(gp->failed_site);
            continue;
        }
        if (config().log) std::fprintf(stderr, "[opt] refused (%s): %u bytes of bytecode\n", gp->why ? gp->why : "?", static_cast<unsigned>(chunk.code.size()));
        return false;
    }
    Graph& g = *gp;
    if (config().dump) dump(g, stderr);
    Emitter em(g);
    std::vector<uint8_t> bytes;
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    if (!em.emit(bytes, entries)) {
        if (config().log) std::fprintf(stderr, "[opt] refused in codegen (%s): %u bytes of bytecode\n", em.why() ? em.why() : "?", static_cast<unsigned>(chunk.code.size()));
        return false;
    }
    void* mem = nullptr;
    size_t size = 0;
    uint8_t* start = nullptr;
    if (!alloc_exec(bytes, mem, size, start)) return false;
    unit.mem = mem;
    unit.mem_size = size;
    unit.code_bytes = bytes.size();
    unit.base = start;
    unit.entry = reinterpret_cast<decltype(unit.entry)>(start);
    unit.rt->sp_counter = g_cfg.safepoint_reload;
    unit.rt->stress = static_cast<int32_t>(config().stress);
    unit.rt->opt_pc = pc;
    all_rts().push_back(unit.rt.get());
    unit.entries = std::move(entries);
    unit.guards = em.guard_kinds;
    unit.descs = std::move(g.descs);
    unit.natives = std::move(g.natives);
    if (g.funcs.size() > 1) {
        // What a deoptimization inside an inlined callee needs to rebuild the interpreter frames it stands for.
        unit.root_vars = g.acc_var + 1;
        unit.inl.resize(g.funcs.size());
        for (size_t k = 1; k < g.funcs.size(); k++) {
            const Func& fn = g.funcs[k];
            unit.inl[k] = {fn.chunk, fn.parent, fn.var_base, fn.nregs, fn.call_pc, fn.call_len};
        }
        unit.inl_states.resize(g.states.size());
        for (size_t k = 0; k < g.states.size(); k++) {
            unit.inl_states[k].func = g.states[k].func;
            unit.inl_states[k].pc = g.states[k].pc;
            if (g.states[k].func == 0) continue;
            for (auto& [v, d] : g.states[k].defs) if (v > g.acc_var) unit.inl_states[k].vars.push_back(v);
        }
        unit.inl_words = g.inl_words;
        unit.inl_depth = g.inl_depth;
        unit.inl_strict = g.inl_strict;
        unit.inl_scratch_mem.reset(new uint64_t[g.nvars - unit.root_vars]());
        unit.rt->inl_scratch = unit.inl_scratch_mem.get();
    }
    if (config().log) std::fprintf(stderr, "[opt] compiled loop at pc %u of %u bytes of bytecode -> %zu bytes of code, %zu entries, %u calls inlined\n",
                                   pc, static_cast<unsigned>(chunk.code.size()), bytes.size(), unit.entries.size(), g.inl_count);
    return true;
}
#endif

}  // namespace opt
