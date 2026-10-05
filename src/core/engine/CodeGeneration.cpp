/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/CodeGeneration.h"
#include "quanta/Embed.h"
#include "quanta/core/engine/Engine.h"

namespace Quanta {

namespace {

namespace E = Embed;

// The strings through the host: transform, then the check. False with the exception pending.
bool run_host(Context& ctx, CodeGenerationHost& host, CompileKind kind, const std::vector<Value>& originals,
              std::vector<std::string>& parts) {
    std::string error;
    if (!host.transform(ctx, kind, originals, parts, error)) {
        if (!ctx.has_exception()) E::ThrowTypeError(ctx, error.empty() ? "Code generation from strings was refused" : error);
        return false;
    }
    if (ctx.has_exception()) return false;
    if (!host.ensure_can_compile(ctx, kind, parts, error)) {
        if (!ctx.has_exception()) {
            E::ThrowError(ctx, "EvalError", error.empty() ? "Code generation from strings disallowed for this context" : error);
        }
        return false;
    }
    return !ctx.has_exception();
}

}

bool prepare_eval_source(Context& ctx, Engine* engine, bool direct, const Value& argument, std::string& code,
                         bool& proceed) {
    CodeGenerationHost* host = engine ? engine->code_generation_host() : nullptr;
    if (!host) {
        proceed = argument.is_string();
        if (proceed) code = argument.to_string();
        return true;
    }
    proceed = true;
    if (argument.is_string()) {
        code = argument.to_string();
    } else if (argument.is_object_like() && host->code_for_eval(ctx, argument, code)) {
        if (ctx.has_exception()) return false;
    } else {
        proceed = false;
        return true;
    }
    std::vector<Value> originals{argument};
    std::vector<std::string> parts{code};
    if (!run_host(ctx, *host, direct ? CompileKind::DirectEval : CompileKind::IndirectEval, originals, parts)) return false;
    code = std::move(parts[0]);
    return true;
}

bool prepare_function_source(Context& ctx, Engine* engine, CompileKind kind, std::span<const Value> args,
                             std::vector<Value>& converted) {
    CodeGenerationHost* host = engine ? engine->code_generation_host() : nullptr;
    if (!host) return true;
    std::vector<Value> originals(args.begin(), args.end());
    std::vector<std::string> parts;
    parts.reserve(args.size());
    E::ValueList keep;   // ToString can run script
    for (const Value& v : args) keep.Append(v);
    for (const Value& v : args) {
        std::string s = E::ToWtf8(ctx, v);
        if (ctx.has_exception()) return false;
        parts.push_back(std::move(s));
    }
    if (!run_host(ctx, *host, kind, originals, parts)) return false;
    converted.clear();
    for (const std::string& p : parts) converted.push_back(Value(p));
    return true;
}

bool prepare_string_source(Context& ctx, Engine* engine, CompileKind kind, const Value& original, std::string& code) {
    CodeGenerationHost* host = engine ? engine->code_generation_host() : nullptr;
    if (!host) return true;
    std::vector<Value> originals{original};
    std::vector<std::string> parts{code};
    if (!run_host(ctx, *host, kind, originals, parts)) return false;
    code = std::move(parts[0]);
    return true;
}

}
