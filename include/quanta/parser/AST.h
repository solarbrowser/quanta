/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_AST_H
#define QUANTA_AST_H

#include "quanta/lexer/Token.h"
#include "quanta/core/runtime/Value.h"
#include "quanta/parser/FunctionExecutable.h"
#include "quanta/parser/ScriptUnit.h"
#include <memory>
#include <vector>
#include <string>
#include "quanta/parser/AstArena.h"
#include "quanta/parser/NamePool.h"
#include <functional>

namespace Quanta {

class BytecodeChunk;

namespace ast_detail {
// Whether a literal being copied keeps pointing at the source it was written
// in. Off by default: a copy is not owned by the unit that parsed it, so the
// pointer would outlive nothing in particular. A copier that keeps the unit
// alive itself turns it on for the length of the copy -- which is what lets a
// function or class written inside one still report its own text.
inline bool& clone_keeps_source_flag() {
    static thread_local bool on = false;
    return on;
}
inline bool clone_keeps_source() { return clone_keeps_source_flag(); }
struct CloneSourceScope {
    bool saved = clone_keeps_source_flag();
    CloneSourceScope() { clone_keeps_source_flag() = true; }
    ~CloneSourceScope() { clone_keeps_source_flag() = saved; }
};

}



class Context;
class Object;
class FunctionExpression;


// Facts about a subtree that the compiler used to learn by walking it. The
// parser knows each of them by the time it finishes the subtree, so they are
// accumulated on the way up instead of rediscovered on the way down. Each is
// monotone: a subtree has the property if any part of it does, minus the
// boundaries that stop it (a nested function owns its own `arguments`, and
// does not pass a `yield` out to its enclosing generator).
//
// kSubtreeComputed says the rest of the word means anything at all. A tree
// that did not come from the parser -- a clone, or a form not migrated yet --
// leaves it clear, and every reader falls back to walking. Trusting a zero
// word would turn "nobody filled this in" into "the answer is no".
enum SubtreeFlags : uint32_t {
    kSubtreeComputed   = 1u << 0,
    kSubtreeSuspend    = 1u << 1,   // await or yield the enclosing body owns
    kSubtreeClosure    = 1u << 2,   // a nested function or class captures the scope
    kSubtreeWith       = 1u << 3,   // a `with` block, which opaques every name in it
    kSubtreeArguments  = 1u << 4,   // the name `arguments`, in the body that owns it
    kSubtreeLexicalDecl = 1u << 5,  // a let/const/class/using/catch binding
    // Set on a function body whose lexical bindings are not all at its top
    // level. Unlike the rest this is not a plain subtree property: the top
    // level of the body itself is deliberately not counted, because those
    // bindings live in the scope the body already has.
    kSubtreeNestedLexical = 1u << 6,
};

class ASTNode {
public:


    enum class Type {
        NUMBER_LITERAL,
        STRING_LITERAL,
        BOOLEAN_LITERAL,
        NULL_LITERAL,
        BIGINT_LITERAL,
        UNDEFINED_LITERAL,
        
        IDENTIFIER,
        ENGINE_HELPER,
        PARAMETER,

        BINARY_EXPRESSION,
        UNARY_EXPRESSION,
        ASSIGNMENT_EXPRESSION,
        CONDITIONAL_EXPRESSION,
        DESTRUCTURING_ASSIGNMENT,
        CALL_EXPRESSION,
        MEMBER_EXPRESSION,
        OPTIONAL_CHAINING_EXPRESSION,
        NULLISH_COALESCING_EXPRESSION,
        NEW_EXPRESSION,
        META_PROPERTY,
        FUNCTION_EXPRESSION,
        ARROW_FUNCTION_EXPRESSION,
        ASYNC_FUNCTION_EXPRESSION,
        AWAIT_EXPRESSION,
        YIELD_EXPRESSION,
        OBJECT_LITERAL,
        ARRAY_LITERAL,
        TEMPLATE_LITERAL,
        REGEX_LITERAL,
        SPREAD_ELEMENT,
        
        EXPRESSION_STATEMENT,
        EMPTY_STATEMENT,
        VARIABLE_DECLARATION,
        VARIABLE_DECLARATOR,
        BLOCK_STATEMENT,
        IF_STATEMENT,
        FOR_STATEMENT,
        FOR_IN_STATEMENT,
        FOR_OF_STATEMENT,
        WHILE_STATEMENT,
        DO_WHILE_STATEMENT,
        WITH_STATEMENT,
        FUNCTION_DECLARATION,
        CLASS_DECLARATION,
        METHOD_DEFINITION,
        CLASS_FIELD,
        CLASS_STATIC_BLOCK,
        RETURN_STATEMENT,
        BREAK_STATEMENT,
        CONTINUE_STATEMENT,
        LABELED_STATEMENT,
        TRY_STATEMENT,
        CATCH_CLAUSE,
        THROW_STATEMENT,
        SWITCH_STATEMENT,
        CASE_CLAUSE,
        
        USING_DECLARATION,

        IMPORT_STATEMENT,
        EXPORT_STATEMENT,
        IMPORT_SPECIFIER,
        EXPORT_SPECIFIER,
        
        JSX_ELEMENT,
        JSX_TEXT,
        JSX_EXPRESSION,
        JSX_ATTRIBUTE,
        
        PROGRAM
    };

protected:
    Type type_;
    // Sits in the padding type_ already leaves before the positions, so the
    // node does not grow by carrying it.
    uint32_t subtree_flags_ = 0;
    Position start_;
    Position end_;

public:
    // Every AST node, of every kind, is allocated here -- see AstArena for why
    // the general allocator is the wrong home for a few hundred thousand small
    // objects that die together.
    static void* operator new(std::size_t n) { return AstArena::take(n); }
    static void operator delete(void* p) noexcept { AstArena::give(p); }
    static void operator delete(void* p, std::size_t) noexcept { AstArena::give(p); }

    ASTNode(Type type, const Position& start, const Position& end)
        : type_(type), start_(start), end_(end) {}
    
    virtual ~ASTNode() = default;
    
    Type get_type() const { return type_; }
    // See SubtreeFlags. Zero means "not computed", never "no".
    uint32_t subtree_flags() const { return subtree_flags_; }
    void set_subtree_flags(uint32_t f) { subtree_flags_ = f; }
    const Position& get_start() const { return start_; }
    const Position& get_end() const { return end_; }
    
    // A Program runs its statements, and the two module declarations keep the
    // records their linking needs. Everything else is compiled: the bytecode
    // is the only thing that executes a node, so reaching this is a gap in the
    // compiler and says so rather than quietly walking a tree.
    virtual Value evaluate(Context& ctx);

    // This node as an expression, compiled. A destructuring pattern's own
    // sub-expressions -- a computed key, a default, a member target's object --
    // reach the engine one at a time, because a suspendable function binds its
    // parameters outside the chunk that owns its body.
    Value evaluate_compiled(Context& ctx);
    virtual std::string to_string() const = 0;
    virtual std::unique_ptr<ASTNode> clone() const = 0;
};

class NumberLiteral : public ASTNode {
private:
    double value_;

public:
    NumberLiteral(double value, const Position& start, const Position& end)
        : ASTNode(Type::NUMBER_LITERAL, start, end), value_(value) {}
    
    double get_value() const { return value_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class StringLiteral : public ASTNode {
private:
    std::string value_;
    bool has_escapes_ = false;

public:
    StringLiteral(const std::string& value, const Position& start, const Position& end,
                  bool has_escapes = false)
        : ASTNode(Type::STRING_LITERAL, start, end), value_(value), has_escapes_(has_escapes) {}

    const std::string& get_value() const { return value_; }
    bool has_escapes() const { return has_escapes_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class BooleanLiteral : public ASTNode {
private:
    bool value_;

public:
    BooleanLiteral(bool value, const Position& start, const Position& end)
        : ASTNode(Type::BOOLEAN_LITERAL, start, end), value_(value) {}
    
    bool get_value() const { return value_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class NullLiteral : public ASTNode {
public:
    NullLiteral(const Position& start, const Position& end)
        : ASTNode(Type::NULL_LITERAL, start, end) {}
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class BigIntLiteral : public ASTNode {
private:
    std::string value_;

public:
    BigIntLiteral(const std::string& value, const Position& start, const Position& end)
        : ASTNode(Type::BIGINT_LITERAL, start, end), value_(value) {}
    
    const std::string& get_value() const { return value_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class UndefinedLiteral : public ASTNode {
public:
    UndefinedLiteral(const Position& start, const Position& end)
        : ASTNode(Type::UNDEFINED_LITERAL, start, end) {}
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class TemplateLiteral : public ASTNode {
public:
    struct Element {
        enum class Type { TEXT, EXPRESSION };
        Type type;
        std::string text;
        std::string raw_text;
        std::unique_ptr<ASTNode> expression;

        Element(const std::string& t) : type(Type::TEXT), text(t), raw_text(t) {}
        Element(const std::string& t, const std::string& raw) : type(Type::TEXT), text(t), raw_text(raw) {}
        Element(std::unique_ptr<ASTNode> expr) : type(Type::EXPRESSION), expression(std::move(expr)) {}
    };

private:
    std::vector<Element> elements_;
    // Slot of this site's frozen call-site object in the collector-rooted store,
    // or -1 until a tag first asks for it. Keying on the node rather than on a
    // pointer map means a site that is freed cannot hand its object to whatever
    // is parsed at the same address later.
    int32_t template_object_slot_ = -1;

public:
    TemplateLiteral(std::vector<Element> elements, const Position& start, const Position& end)
        : ASTNode(Type::TEMPLATE_LITERAL, start, end), elements_(std::move(elements)) {}

    ~TemplateLiteral() override;

    // GetTemplateObject: every evaluation of one site hands the tag the same
    // object, which is what lets a tag cache against its argument's identity.
    Value cached_template_object() const;
    void cache_template_object(const Value& obj);
    
    const std::vector<Element>& get_elements() const { return elements_; }

    // Shared with BytecodeCompiler/VM (Op::ToTemplateString) so both engines
    // stringify an interpolated value identically: an own/inherited toString
    // is preferred over ToPrimitive/valueOf, matching this class's existing
    // (non-ToPrimitive) conversion rather than the general `+` coercion.
    static std::string stringify_element(Context& ctx, const Value& v);

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class RegexLiteral : public ASTNode {
private:
    std::string pattern_;
    std::string flags_;

public:
    RegexLiteral(const std::string& pattern, const std::string& flags,
                 const Position& start, const Position& end)
        : ASTNode(Type::REGEX_LITERAL, start, end), pattern_(pattern), flags_(flags) {}
    
    const std::string& get_pattern() const { return pattern_; }
    const std::string& get_flags() const { return flags_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class Identifier : public ASTNode {
private:
    // The name lives in the pool, once, and this is which one. Nearly half of
    // every tree is identifier nodes, and holding the text in each of them was
    // both the largest thing the tree carried and what kept the node a size
    // class larger than it needed to be.
    uint32_t name_id_ = 0;
    bool has_escaped_keyword_ = false;

public:
    Identifier(const std::string& name, const Position& start, const Position& end)
        : ASTNode(Type::IDENTIFIER, start, end), name_id_(NamePool::intern(name)) {}

    const std::string& get_name() const { return NamePool::text(name_id_); }
    uint32_t get_name_id() const { return name_id_; }
    bool has_escaped_keyword() const { return has_escaped_keyword_; }
    void set_escaped_keyword(bool v) { has_escaped_keyword_ = v; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

// The callee of a call the engine synthesised for itself: a class field
// definition, a private-field add, the class-field-initialiser flag, naming an
// anonymous field value, import.source. These were Identifiers naming global
// functions -- __deffield__ and friends -- which put six engine functions on
// globalThis where script could list them, call them, and (because an
// Identifier resolves through the scope chain) SHADOW them: `var __deffield__
// = f` took over class field definition, and `var __pfadd__ = f` stopped
// private fields from existing at all. Naming the helper instead of looking it
// up removes every one of those; the functions themselves now live in the
// global object's internal slots, which no name can reach.
class EngineHelper : public ASTNode {
public:
    enum class Kind : uint8_t {
        DefineField, PrivateFieldAdd, SetFunctionName,
        ClassFieldInitEnter, ClassFieldInitExit, ImportSource, ImportDefer,
        // A computed instance-field key, resolved once when the class is built
        // and read back by index at construction time -- baking the resolved
        // string into the constructor would make its body differ per evaluation.
        ClassFieldKey
    };

    EngineHelper(Kind kind, const Position& start, const Position& end)
        : ASTNode(Type::ENGINE_HELPER, start, end), kind_(kind) {}

    Kind get_kind() const { return kind_; }
    // The slot the helper is stored under, and the name it reports in a stack
    // trace. Defined in AST.cpp next to the resolution itself.
    static const char* slot_name(Kind kind);

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;

private:
    Kind kind_;
};

class BinaryExpression : public ASTNode {
public:
    enum class Operator {
        ADD,
        SUBTRACT,
        MULTIPLY,
        DIVIDE,
        MODULO,
        EXPONENT,
        
        EQUAL,
        NOT_EQUAL,
        STRICT_EQUAL,
        STRICT_NOT_EQUAL,
        LESS_THAN,
        GREATER_THAN,
        LESS_EQUAL,
        GREATER_EQUAL,
        INSTANCEOF,
        IN,
        
        LOGICAL_AND,
        LOGICAL_OR,
        
        COMMA,
        
        BITWISE_AND,
        BITWISE_OR,
        BITWISE_XOR,
        LEFT_SHIFT,
        RIGHT_SHIFT,
        UNSIGNED_RIGHT_SHIFT,
        
        ASSIGN,
        PLUS_ASSIGN,
        MINUS_ASSIGN,
        MULTIPLY_ASSIGN,
        DIVIDE_ASSIGN,
        MODULO_ASSIGN,
        BITWISE_AND_ASSIGN,
        BITWISE_OR_ASSIGN,
        BITWISE_XOR_ASSIGN,
        LEFT_SHIFT_ASSIGN,
        RIGHT_SHIFT_ASSIGN,
        UNSIGNED_RIGHT_SHIFT_ASSIGN
    };

private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    Operator operator_;

public:
    BinaryExpression(std::unique_ptr<ASTNode> left, Operator op, std::unique_ptr<ASTNode> right,
                    const Position& start, const Position& end)
        : ASTNode(Type::BINARY_EXPRESSION, start, end), 
          left_(std::move(left)), right_(std::move(right)), operator_(op) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    Operator get_operator() const { return operator_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string operator_to_string(Operator op);
    static Operator token_type_to_operator(TokenType type);
    static int get_precedence(Operator op);
    static bool is_right_associative(Operator op);
    // Full-semantics binary op on already-evaluated operands (number fast path,
    // ToPrimitive, BigInt, string concat, relational). Shared by the tree-walker
    // and the bytecode VM so the two never drift. Short-circuit forms
    // (&&, ||, comma) and assignment forms are NOT handled here.
    static Value apply_operator(Context& ctx, Operator op, const Value& left, const Value& right);
};

class UnaryExpression : public ASTNode {
public:
    enum class Operator {
        PLUS,
        MINUS,
        LOGICAL_NOT,
        BITWISE_NOT,
        TYPEOF,
        VOID,
        DELETE,
        PRE_INCREMENT,
        POST_INCREMENT,
        PRE_DECREMENT,
        POST_DECREMENT
    };

private:
    std::unique_ptr<ASTNode> operand_;
    Operator operator_;
    bool prefix_;

public:
    UnaryExpression(Operator op, std::unique_ptr<ASTNode> operand, bool prefix,
                   const Position& start, const Position& end)
        : ASTNode(Type::UNARY_EXPRESSION, start, end), 
          operand_(std::move(operand)), operator_(op), prefix_(prefix) {}
    
    ASTNode* get_operand() const { return operand_.get(); }
    Operator get_operator() const { return operator_; }
    bool is_prefix() const { return prefix_; }

    // ToNumeric with full valueOf/toString side effects; shared with the
    // bytecode VM's Inc/Dec/ToNumber so ++/-- semantics never drift.
    static Value to_numeric(Context& ctx, const Value& v);
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string operator_to_string(Operator op);
};

class ConditionalExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> consequent_;
    std::unique_ptr<ASTNode> alternate_;

public:
    ConditionalExpression(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> consequent,
                         std::unique_ptr<ASTNode> alternate, const Position& start, const Position& end)
        : ASTNode(Type::CONDITIONAL_EXPRESSION, start, end),
          test_(std::move(test)), consequent_(std::move(consequent)), alternate_(std::move(alternate)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_consequent() const { return consequent_.get(); }
    ASTNode* get_alternate() const { return alternate_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class AssignmentExpression : public ASTNode {
public:
    enum class Operator {
        ASSIGN,
        PLUS_ASSIGN,
        MINUS_ASSIGN,
        MUL_ASSIGN,
        DIV_ASSIGN,
        MOD_ASSIGN,
        BITWISE_AND_ASSIGN,
        BITWISE_OR_ASSIGN,
        BITWISE_XOR_ASSIGN,
        LEFT_SHIFT_ASSIGN,
        RIGHT_SHIFT_ASSIGN,
        UNSIGNED_RIGHT_SHIFT_ASSIGN,
        LOGICAL_AND_ASSIGN,
        LOGICAL_OR_ASSIGN,
        NULLISH_ASSIGN
    };

private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    Operator operator_;
    bool lhs_is_paren_ = false;

public:
    AssignmentExpression(std::unique_ptr<ASTNode> left, Operator op, std::unique_ptr<ASTNode> right,
                        const Position& start, const Position& end, bool lhs_is_paren = false)
        : ASTNode(Type::ASSIGNMENT_EXPRESSION, start, end),
          left_(std::move(left)), right_(std::move(right)), operator_(op), lhs_is_paren_(lhs_is_paren) {}

    bool is_lhs_paren() const { return lhs_is_paren_; }
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    Operator get_operator() const { return operator_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;

    // ES6: Destructuring assignment helpers
};

class DestructuringAssignment : public ASTNode {
public:
    enum class Type {
        ARRAY,
        OBJECT
    };
    


private:
    std::unique_ptr<ASTNode> source_;
    Type type_;

    // A nested pattern (`outer: { inner, deep = expr }`) is parsed as its own
    // throwaway DestructuringAssignment, then flattened into a colon-encoded
    // string (see Parser::generate_proper_nested_pattern) walked at runtime by
    // handle_infinite_depth_destructuring -- that flattening has no room for
    // an ASTNode* default expression, so the parser moves any default it
    // finds at any nesting depth up into this name-keyed side table on the
    // OUTERMOST DestructuringAssignment instead (see parse_destructuring_pattern's
    // "escaped_nested_defaults" and add_nested_default_value call sites).

    // The pattern itself, held as the ObjectLiteral/ArrayLiteral the cover
    // grammar already parses (`{a, b: {c}, d = 1}` parses as an object literal
    // and is reinterpreted here). That literal IS a proper tree, which the
    // flat targets_/property_mappings_ representation was not: nested patterns
    // used to be flattened into delimiter-joined identifier strings, leaving
    // nowhere for a nested default's ASTNode* and no way to express more than
    // one level. AssignmentExpression's own destructuring already walks this
    // shape correctly, so both forms can share one binder.
    std::unique_ptr<ASTNode> pattern_literal_;

public:
    ASTNode* get_pattern_literal() const { return pattern_literal_.get(); }
    void set_pattern_literal(std::unique_ptr<ASTNode> lit) { pattern_literal_ = std::move(lit); }

    // Every name this pattern binds, in source order, at any depth. Replaces
    // the callers that walked targets_ and property_mappings_ separately and
    // then had to filter out the property keys that shared that storage.
    void collect_bound_names(std::vector<std::string>& out) const;

    // Every expression a pattern contains: defaults and computed keys, at any
    // depth. For the closure/env-residency analyses, which need to know what a
    // pattern can reference without caring about its shape.
    void for_each_expression(const std::function<void(const ASTNode*)>& fn) const;

    DestructuringAssignment(std::vector<std::unique_ptr<Identifier>>,
                           std::unique_ptr<ASTNode> source, Type type,
                           const Position& start, const Position& end)
        : ASTNode(ASTNode::Type::DESTRUCTURING_ASSIGNMENT, start, end),
          source_(std::move(source)), type_(type) {}

    ASTNode* get_source() const { return source_.get(); }
    Type get_type() const { return type_; }
    void set_source(std::unique_ptr<ASTNode> source) { source_ = std::move(source); }

    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};


class CallExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> callee_;
    std::vector<std::unique_ptr<ASTNode>> arguments_;
    bool is_tagged_template_ = false;
    bool is_optional_ = false;

public:
    CallExpression(std::unique_ptr<ASTNode> callee, std::vector<std::unique_ptr<ASTNode>> arguments,
                  const Position& start, const Position& end, bool is_optional = false)
        : ASTNode(Type::CALL_EXPRESSION, start, end),
          callee_(std::move(callee)), arguments_(std::move(arguments)), is_optional_(is_optional) {}

    ASTNode* get_callee() const { return callee_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_arguments() const { return arguments_; }
    size_t argument_count() const { return arguments_.size(); }
    void set_tagged_template(bool v) { is_tagged_template_ = v; }
    bool is_tagged_template() const { return is_tagged_template_; }
    bool is_optional() const { return is_optional_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};


class MemberExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> object_;
    std::unique_ptr<ASTNode> property_;
    bool computed_;


public:
    MemberExpression(std::unique_ptr<ASTNode> object, std::unique_ptr<ASTNode> property,
                    bool computed, const Position& start, const Position& end)
        : ASTNode(Type::MEMBER_EXPRESSION, start, end),
          object_(std::move(object)), property_(std::move(property)), computed_(computed) {}

    ASTNode* get_object() const { return object_.get(); }
    ASTNode* get_property() const { return property_.get(); }
    bool is_computed() const { return computed_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class OptionalChainingExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> object_;
    std::unique_ptr<ASTNode> property_;
    bool computed_;

public:
    OptionalChainingExpression(std::unique_ptr<ASTNode> object, std::unique_ptr<ASTNode> property, 
                              bool computed, const Position& start, const Position& end)
        : ASTNode(Type::OPTIONAL_CHAINING_EXPRESSION, start, end), 
          object_(std::move(object)), property_(std::move(property)), computed_(computed) {}
    
    ASTNode* get_object() const { return object_.get(); }
    ASTNode* get_property() const { return property_.get(); }
    bool is_computed() const { return computed_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class NullishCoalescingExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;

public:
    NullishCoalescingExpression(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                               const Position& start, const Position& end)
        : ASTNode(Type::NULLISH_COALESCING_EXPRESSION, start, end), 
          left_(std::move(left)), right_(std::move(right)) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class NewExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> constructor_;
    std::vector<std::unique_ptr<ASTNode>> arguments_;

public:
    NewExpression(std::unique_ptr<ASTNode> constructor, 
                  std::vector<std::unique_ptr<ASTNode>> arguments,
                  const Position& start, const Position& end)
        : ASTNode(Type::NEW_EXPRESSION, start, end), 
          constructor_(std::move(constructor)), arguments_(std::move(arguments)) {}
    
    ASTNode* get_constructor() const { return constructor_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_arguments() const { return arguments_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class MetaProperty : public ASTNode {
private:
    std::string meta_;
    std::string property_;

public:
    MetaProperty(const std::string& meta, const std::string& property,
                 const Position& start, const Position& end)
        : ASTNode(Type::META_PROPERTY, start, end),
          meta_(meta), property_(property) {}

    const std::string& get_meta() const { return meta_; }
    const std::string& get_property() const { return property_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};


class VariableDeclarator : public ASTNode {
public:
    enum class Kind {
        VAR,
        LET,
        CONST
    };

private:
    std::unique_ptr<Identifier> id_;
    std::unique_ptr<ASTNode> init_;
    Kind kind_;

public:
    VariableDeclarator(std::unique_ptr<Identifier> id, std::unique_ptr<ASTNode> init, Kind kind,
                      const Position& start, const Position& end)
        : ASTNode(Type::VARIABLE_DECLARATOR, start, end), 
          id_(std::move(id)), init_(std::move(init)), kind_(kind) {}
    
    Identifier* get_id() const { return id_.get(); }
    ASTNode* get_init() const { return init_.get(); }
    Kind get_kind() const { return kind_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string kind_to_string(Kind kind);
};

/**
 * Variable declaration statement (e.g., "var x = 5;", "let y;")
 */
class VariableDeclaration : public ASTNode {
private:
    std::vector<std::unique_ptr<VariableDeclarator>> declarations_;
    VariableDeclarator::Kind kind_;

public:
    VariableDeclaration(std::vector<std::unique_ptr<VariableDeclarator>> declarations, 
                       VariableDeclarator::Kind kind, const Position& start, const Position& end)
        : ASTNode(Type::VARIABLE_DECLARATION, start, end), 
          declarations_(std::move(declarations)), kind_(kind) {}
    
    const std::vector<std::unique_ptr<VariableDeclarator>>& get_declarations() const { return declarations_; }
    VariableDeclarator::Kind get_kind() const { return kind_; }
    size_t declaration_count() const { return declarations_.size(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

struct UsingBinding {
    std::string name;
    std::unique_ptr<ASTNode> initializer;
    UsingBinding(std::string n, std::unique_ptr<ASTNode> init)
        : name(std::move(n)), initializer(std::move(init)) {}
};

class UsingDeclaration : public ASTNode {
private:
    std::vector<UsingBinding> bindings_;
    bool is_await_;  // true for 'await using'
public:
    UsingDeclaration(std::vector<UsingBinding> bindings, bool is_await,
                     const Position& start, const Position& end)
        : ASTNode(Type::USING_DECLARATION, start, end),
          bindings_(std::move(bindings)), is_await_(is_await) {}

    const std::vector<UsingBinding>& get_bindings() const { return bindings_; }
    bool is_await() const { return is_await_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Block statement (e.g., "{ ... }")
 */
class BlockStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> statements_;
    // Blocks without their own block-scoped bindings skip the per-evaluation Environment.
    mutable int8_t needs_scope_ = -1;
    // Directive prologue never changes after parsing -- same tri-state idiom
    // as needs_scope_ above.
    mutable int8_t use_strict_cached_ = -1;
    // Whether this block (as a closure body) syntactically contains a direct
    // `eval(...)` call -- also parse-time-immutable, same tri-state idiom.
    // Defined in language.cpp (contains_direct_eval is file-local there).
    mutable int8_t contains_eval_cached_ = -1;

public:
    BlockStatement(std::vector<std::unique_ptr<ASTNode>> statements, const Position& start, const Position& end)
        : ASTNode(Type::BLOCK_STATEMENT, start, end), statements_(std::move(statements)) {}

    const std::vector<std::unique_ptr<ASTNode>>& get_statements() const { return statements_; }
    size_t statement_count() const { return statements_.size(); }

    void check_use_strict_directive(Context& ctx);
    // Side-effect-free query version of the same directive-prologue scan, for
    // callers that don't have (or don't want to mutate) a Context.
    bool has_use_strict_directive() const;
    // Cached wrapper around contains_direct_eval(this) (language.cpp) -- a
    // closure body's direct-eval-containment is fixed at parse time, so this
    // avoids re-walking the whole body on every closure instantiation.
    bool has_direct_eval_cached() const;
    bool needs_own_scope() const;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * If statement (e.g., "if (condition) statement", "if (condition) statement else statement")
 */
class IfStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> consequent_;
    std::unique_ptr<ASTNode> alternate_;

public:
    IfStatement(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> consequent, 
               std::unique_ptr<ASTNode> alternate, const Position& start, const Position& end)
        : ASTNode(Type::IF_STATEMENT, start, end), 
          test_(std::move(test)), consequent_(std::move(consequent)), alternate_(std::move(alternate)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_consequent() const { return consequent_.get(); }
    ASTNode* get_alternate() const { return alternate_.get(); }
    bool has_alternate() const { return alternate_ != nullptr; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * For loop statement (e.g., "for (let i = 0; i < 10; i++) { ... }")
 */
class ForStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> init_;
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> update_;
    std::unique_ptr<ASTNode> body_;
    int init_decl_kind_; // -1=none, 0=var, 1=let, 2=const (set when init is a destructuring pattern -- a plain VariableDeclaration carries its own kind instead)

public:
    ForStatement(std::unique_ptr<ASTNode> init, std::unique_ptr<ASTNode> test,
                 std::unique_ptr<ASTNode> update, std::unique_ptr<ASTNode> body,
                 const Position& start, const Position& end,
                 int init_decl_kind = -1)
        : ASTNode(Type::FOR_STATEMENT, start, end),
          init_(std::move(init)), test_(std::move(test)),
          update_(std::move(update)), body_(std::move(body)),
          init_decl_kind_(init_decl_kind) {}

    ASTNode* get_init() const { return init_.get(); }
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_update() const { return update_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    int get_init_decl_kind() const { return init_decl_kind_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * For...in loop statement (e.g., "for (const key in obj) { ... }")
 */
class ForInStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    std::unique_ptr<ASTNode> body_;
    int left_decl_kind_; // -1=none, 0=var, 1=let, 2=const (for destructuring left side)
public:
    ForInStatement(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                   std::unique_ptr<ASTNode> body, const Position& start, const Position& end,
                   int left_decl_kind = -1)
        : ASTNode(Type::FOR_IN_STATEMENT, start, end),
          left_(std::move(left)), right_(std::move(right)), body_(std::move(body)),
          left_decl_kind_(left_decl_kind) {}

    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    int get_left_decl_kind() const { return left_decl_kind_; }

    // Shared with the VM (Op::CreateForInKeys): own enumerable keys, then
    // the prototype chain's, skipping anything already seen at a closer
    // level. False only for a Proxy ownKeys trap violation.
    static bool collect_keys(Context& ctx, Object* obj, std::vector<std::string>& out_keys);

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ForOfStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    std::unique_ptr<ASTNode> body_;
    bool is_await_;
    int left_decl_kind_; // -1=none, 0=var, 1=let, 2=const (for destructuring left side)
public:
    ForOfStatement(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                   std::unique_ptr<ASTNode> body, bool is_await, const Position& start, const Position& end,
                   int left_decl_kind = -1)
        : ASTNode(Type::FOR_OF_STATEMENT, start, end),
          left_(std::move(left)), right_(std::move(right)), body_(std::move(body)),
          is_await_(is_await), left_decl_kind_(left_decl_kind) {}

    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    bool is_await() const { return is_await_; }
    int get_left_decl_kind() const { return left_decl_kind_; }

    // Shared with the VM (Op::GetIterator/IteratorNextOrJump/IteratorClose),
    // sync/non-destructuring/non-await path only. False (exception set)
    // leaves the caller not calling iterator_close (matching spec: don't
    // close if GetIterator/next itself threw).
    static bool get_iterator(Context& ctx, const Value& iterable, Value& out_iterator, Value& out_next_fn);
    static bool iterator_step(Context& ctx, const Value& iterator, Value& next_fn,
                               bool& out_done, Value& out_value);
    // validate_result: check return()'s result is an Object (IteratorClose-
    // after-break rule). is_pending/pending_exception: an in-flight
    // exception that return() must not clobber.
    static void iterator_close(Context& ctx, const Value& iterator, bool validate_result,
                                bool is_pending, const Value& pending_exception);

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class WhileStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> body_;

public:
    WhileStatement(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> body,
                   const Position& start, const Position& end)
        : ASTNode(Type::WHILE_STATEMENT, start, end), 
          test_(std::move(test)), body_(std::move(body)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class DoWhileStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> body_;
    std::unique_ptr<ASTNode> test_;

public:
    DoWhileStatement(std::unique_ptr<ASTNode> body, std::unique_ptr<ASTNode> test,
                     const Position& start, const Position& end)
        : ASTNode(Type::DO_WHILE_STATEMENT, start, end),
          body_(std::move(body)), test_(std::move(test)) {}

    ASTNode* get_body() const { return body_.get(); }
    ASTNode* get_test() const { return test_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class WithStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> object_;
    std::unique_ptr<ASTNode> body_;

public:
    WithStatement(std::unique_ptr<ASTNode> object, std::unique_ptr<ASTNode> body,
                  const Position& start, const Position& end)
        : ASTNode(Type::WITH_STATEMENT, start, end),
          object_(std::move(object)), body_(std::move(body)) {}

    ASTNode* get_object() const { return object_.get(); }
    ASTNode* get_body() const { return body_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class Parameter : public ASTNode {
private:
    std::unique_ptr<Identifier> name_;
    std::unique_ptr<ASTNode> default_value_;
    std::unique_ptr<ASTNode> destructuring_pattern_;
    // A suspendable function binds its parameters outside the chunk that owns
    // its body, so the default is a chunk of its own. It is decl-site data
    // like the expression it came from: compiled once, shared by every call.
    mutable std::unique_ptr<BytecodeChunk> default_chunk_;
    mutable bool default_chunk_tried_ = false;
    mutable std::unique_ptr<BytecodeChunk> pattern_chunk_;
    mutable bool pattern_chunk_tried_ = false;
    bool is_rest_;

public:
    Parameter(std::unique_ptr<Identifier> name, std::unique_ptr<ASTNode> default_value,
              bool is_rest, const Position& start, const Position& end)
        : ASTNode(Type::PARAMETER, start, end),
          name_(std::move(name)), default_value_(std::move(default_value)), is_rest_(is_rest) {}

    Identifier* get_name() const { return name_.get(); }
    ASTNode* get_default_value() const { return default_value_.get(); }
    BytecodeChunk* default_chunk() const { return default_chunk_.get(); }
    void set_default_chunk(std::unique_ptr<BytecodeChunk> c) const { default_chunk_ = std::move(c); }
    bool default_chunk_tried() const { return default_chunk_tried_; }
    void mark_default_chunk_tried() const { default_chunk_tried_ = true; }
    BytecodeChunk* pattern_chunk() const { return pattern_chunk_.get(); }
    void set_pattern_chunk(std::unique_ptr<BytecodeChunk> c) const { pattern_chunk_ = std::move(c); }
    bool pattern_chunk_tried() const { return pattern_chunk_tried_; }
    void mark_pattern_chunk_tried() const { pattern_chunk_tried_ = true; }
    bool has_default() const { return default_value_ != nullptr; }
    bool is_rest() const { return is_rest_; }
    ASTNode* get_destructuring_pattern() const { return destructuring_pattern_.get(); }
    bool has_destructuring() const { return destructuring_pattern_ != nullptr; }
    void set_destructuring_pattern(std::unique_ptr<ASTNode> pattern) { destructuring_pattern_ = std::move(pattern); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class FunctionDeclaration : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;
    bool is_async_;
    bool is_generator_;
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;
    // Built once on first evaluate(), reused by every instantiation -- same
    // idiom as FunctionExpression::cached_executable_ (see
    // FunctionExecutable's own doc comment for why a durable clone, not a
    // borrow, is required).
    mutable ExecutableRef<FunctionExecutable> cached_executable_;
    // Which parse tree this literal belongs to, recorded when the node was
    // built (see ScriptUnit::BuildScope). Null for trees built outside a unit,
    // which still take their own clone.
    ScriptUnit* owning_unit_ = ScriptUnit::building();

public:
    FunctionDeclaration(std::unique_ptr<Identifier> id,
                       std::vector<std::unique_ptr<Parameter>> params,
                       std::unique_ptr<BlockStatement> body,
                       const Position& start, const Position& end,
                       bool is_async = false, bool is_generator = false)
        : ASTNode(Type::FUNCTION_DECLARATION, start, end),
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)), is_async_(is_async), is_generator_(is_generator) {}

    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_async() const { return is_async_; }
    bool is_generator() const { return is_generator_; }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Drops the body subtree, leaving the range to read it back from. A body
    // holding inner literals reads back correctly too: an executable is found
    // by source offset rather than by node, and the literals inside are
    // stepped over rather than read again.
    void release_body() { body_.reset(); }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }
    // Keyed on where the body opens in the source, not on this node's address:
    // a body parsed back from the source is a different node for the same
    // declaration, and the executable has to be the same one either way. A
    // literal with no body range of its own keeps the node-local copy.
    const ExecutableRef<FunctionExecutable>& get_cached_executable() const {
        if (owning_unit_ && has_body_token_range()) {
            return owning_unit_->executable_at(body_src_first_);
        }
        return cached_executable_;
    }
    ScriptUnit* owning_unit() const { return owning_unit_; }
    void set_cached_executable(ExecutableRef<FunctionExecutable> exe) const {
        if (owning_unit_ && has_body_token_range()) {
            owning_unit_->set_executable_at(body_src_first_, std::move(exe));
            return;
        }
        cached_executable_ = std::move(exe);
    }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ClassDeclaration : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::unique_ptr<ASTNode> superclass_;
    std::unique_ptr<BlockStatement> body_;
    // Which parse tree this node belongs to, recorded when it was built
    // (see ScriptUnit::BuildScope) so its source range can be resolved.
    ScriptUnit* owning_unit_ = ScriptUnit::building();
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;
    std::string inferred_name_;
    bool is_expression_ = false;
    // The constructor executable for this class site. Building it means cloning
    // the constructor body -- and, when the class has fields, synthesising a new
    // body around them -- which a class defined inside a repeatedly-called
    // function otherwise paid on every evaluation. Only shared when the result
    // is the same every time: a private-method brand slot embeds the prototype
    // address, and a computed field key is resolved per evaluation, so neither
    // is cacheable.
    mutable ExecutableRef<FunctionExecutable> cached_ctor_exe_;


public:
    void set_is_expression(bool v) { is_expression_ = v; }
    bool is_expression() const { return is_expression_; }
    // NamedEvaluation: an anonymous class expression's name from its binding site,
    // set BEFORE evaluation so static initializers already see it via this.name
    // (spec: SetFunctionName runs before static fields). Creates no name binding.
    void set_inferred_name(const std::string& n) { inferred_name_ = n; }
    const std::string& get_inferred_name() const { return inferred_name_; }

    ClassDeclaration(std::unique_ptr<Identifier> id,
                    std::unique_ptr<ASTNode> superclass,
                    std::unique_ptr<BlockStatement> body,
                    const Position& start, const Position& end)
        : ASTNode(Type::CLASS_DECLARATION, start, end),
          id_(std::move(id)), superclass_(std::move(superclass)), body_(std::move(body)) {}

    ClassDeclaration(std::unique_ptr<Identifier> id,
                    std::unique_ptr<BlockStatement> body,
                    const Position& start, const Position& end)
        : ASTNode(Type::CLASS_DECLARATION, start, end),
          id_(std::move(id)), superclass_(nullptr), body_(std::move(body)) {}

    Identifier* get_id() const { return id_.get(); }
    ASTNode* get_superclass() const { return superclass_.get(); }
    BlockStatement* get_body() const { return body_.get(); }
    bool has_superclass() const { return superclass_ != nullptr; }
    // Same key, its own table -- a class site has both.
    const ExecutableRef<FunctionExecutable>& get_cached_ctor_exe() const {
        if (owning_unit_ && has_body_token_range()) {
            return owning_unit_->ctor_executable_at(body_src_first_);
        }
        return cached_ctor_exe_;
    }
    void set_cached_ctor_exe(ExecutableRef<FunctionExecutable> e) const {
        if (owning_unit_ && has_body_token_range()) {
            owning_unit_->set_ctor_executable_at(body_src_first_, std::move(e));
            return;
        }
        cached_ctor_exe_ = std::move(e);
    }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Drops the body subtree, leaving the range to read it back from. A body
    // holding inner literals reads back correctly too: an executable is found
    // by source offset rather than by node, and the literals inside are
    // stepped over rather than read again.
    void release_body() { body_.reset(); }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }

    // Builds the class. evaluate() is the tree-walker's way in; Op::DefineClass
    // calls this directly, so the node is read as a description rather than
    // walked.
    Value define_class(Context& ctx);
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class MethodDefinition : public ASTNode {
public:
    enum Kind {
        CONSTRUCTOR,
        METHOD,
        STATIC_METHOD,
        GETTER,
        SETTER
    };

private:
    std::unique_ptr<ASTNode> key_;
    std::unique_ptr<FunctionExpression> value_;
    Kind kind_;
    bool is_static_;
    bool computed_;
    // Which parse tree this node belongs to, recorded when it was built
    // (see ScriptUnit::BuildScope) so its source range can be resolved.
    ScriptUnit* owning_unit_ = ScriptUnit::building();
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;

public:
    MethodDefinition(std::unique_ptr<ASTNode> key,
                    std::unique_ptr<FunctionExpression> value,
                    Kind kind,
                    bool is_static,
                    bool computed,
                    const Position& start, const Position& end)
        : ASTNode(Type::METHOD_DEFINITION, start, end),
          key_(std::move(key)), value_(std::move(value)), kind_(kind), is_static_(is_static), computed_(computed) {}

    ASTNode* get_key() const { return key_.get(); }
    bool is_computed() const { return computed_; }
    FunctionExpression* get_value() const { return value_.get(); }
    Kind get_kind() const { return kind_; }
    bool is_static() const { return is_static_; }
    bool is_constructor() const { return kind_ == CONSTRUCTOR; }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ClassField : public ASTNode {
private:
    std::unique_ptr<ASTNode> key_;
    std::unique_ptr<ASTNode> value_;
    bool is_static_;
    bool computed_;
public:
    ClassField(std::unique_ptr<ASTNode> key, std::unique_ptr<ASTNode> value,
               bool is_static, bool computed,
               const Position& start, const Position& end)
        : ASTNode(Type::CLASS_FIELD, start, end),
          key_(std::move(key)), value_(std::move(value)),
          is_static_(is_static), computed_(computed) {}
    ASTNode* get_key() const { return key_.get(); }
    ASTNode* get_value() const { return value_.get(); }
    bool is_static() const { return is_static_; }
    bool is_computed() const { return computed_; }
    std::string to_string() const override { return "[ClassField]"; }
    std::unique_ptr<ASTNode> clone() const override;
};

class ClassStaticBlock : public ASTNode {
private:
    std::unique_ptr<BlockStatement> body_;
public:
    ClassStaticBlock(std::unique_ptr<BlockStatement> body,
                     const Position& start, const Position& end)
        : ASTNode(Type::CLASS_STATIC_BLOCK, start, end), body_(std::move(body)) {}
    BlockStatement* get_body() const { return body_.get(); }
    std::string to_string() const override { return "[ClassStaticBlock]"; }
    std::unique_ptr<ASTNode> clone() const override;
};


class FunctionExpression : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;
    bool is_generator_;
    bool is_async_;
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;
    bool is_decl_form_ = false; // `export default function fn(){}`: HoistableDeclaration, not NamedEvaluation
    bool is_method_shorthand_ = false; // `{m(){}}`/`get x(){}`: non-constructible, skip the .prototype build

    // params_ never changes after parsing, so both are pure functions of it --
    // computed once on first evaluate() (clone-elision path) instead of
    // rebuilt on every closure instantiation. Mirrors BlockStatement's own
    // needs_scope_ tri-state cache idiom.
    mutable std::vector<std::string> cached_param_names_;
    mutable bool param_cache_ready_ = false;
    mutable size_t cached_spec_length_ = 0;
    // -1 unknown, 0 needs the outer environment kept alive, 1 doesn't --
    // see closure_needs_outer_environment's doc comment (BytecodeCompiler.h).
    // Resolved once per literal site, reused by every instantiation (the
    // same closure body is scanned whether this expression runs once or
    // 100000 times).
    mutable int8_t needs_outer_env_state_ = -1;

    // Built once on first evaluate(), reused by every instantiation --
    // FunctionExecutable's own doc comment explains why a durable clone
    // (not a borrow) is required. Same lazy-cache idiom as
    // cached_param_names_ above.
    mutable ExecutableRef<FunctionExecutable> cached_executable_;
    // Which parse tree this literal belongs to, recorded when the node was
    // built (see ScriptUnit::BuildScope). Null for trees built outside a unit,
    // which still take their own clone.
    ScriptUnit* owning_unit_ = ScriptUnit::building();

public:
    FunctionExpression(std::unique_ptr<Identifier> id,
                      std::vector<std::unique_ptr<Parameter>> params,
                      std::unique_ptr<BlockStatement> body,
                      const Position& start, const Position& end,
                      bool is_generator = false,
                      bool is_async = false)
        : ASTNode(Type::FUNCTION_EXPRESSION, start, end),
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)),
          is_generator_(is_generator), is_async_(is_async) {}

    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_named() const { return id_ != nullptr; }
    bool is_generator() const { return is_generator_; }
    bool is_async() const { return is_async_; }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Drops the body subtree, leaving the range to read it back from. A body
    // holding inner literals reads back correctly too: an executable is found
    // by source offset rather than by node, and the literals inside are
    // stepped over rather than read again.
    void release_body() { body_.reset(); }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }
    void set_decl_form(bool v) { is_decl_form_ = v; }
    bool is_decl_form() const { return is_decl_form_; }
    void set_method_shorthand(bool v) { is_method_shorthand_ = v; }
    bool is_method_shorthand() const { return is_method_shorthand_; }

    // Lazily computed, cached forever after (params_ is immutable post-parse).
    const std::vector<std::string>& get_cached_param_names() const {
        ensure_param_cache();
        return cached_param_names_;
    }
    size_t get_cached_spec_length() const {
        ensure_param_cache();
        return cached_spec_length_;
    }
    // Raw cache slot for closure_needs_outer_environment's result (computed
    // in FunctionExpression::evaluate, which already links against
    // BytecodeCompiler -- kept out of this parser-only header to avoid a
    // parser->VM include). -1 unknown, 0 needs env, 1 doesn't.
    int8_t get_needs_outer_env_state() const { return needs_outer_env_state_; }
    void set_needs_outer_env_state(int8_t v) const { needs_outer_env_state_ = v; }

    // Built lazily by FunctionExpression::evaluate on first evaluation of
    // this node; every later evaluation reuses the same shared_ptr instead
    // of cloning body_/params_ again.
    // Keyed on where the body opens in the source, not on this node's address:
    // a body parsed back from the source is a different node for the same
    // declaration, and the executable has to be the same one either way. A
    // literal with no body range of its own keeps the node-local copy.
    const ExecutableRef<FunctionExecutable>& get_cached_executable() const {
        if (owning_unit_ && has_body_token_range()) {
            return owning_unit_->executable_at(body_src_first_);
        }
        return cached_executable_;
    }
    ScriptUnit* owning_unit() const { return owning_unit_; }
    void set_cached_executable(ExecutableRef<FunctionExecutable> exe) const {
        if (owning_unit_ && has_body_token_range()) {
            owning_unit_->set_executable_at(body_src_first_, std::move(exe));
            return;
        }
        cached_executable_ = std::move(exe);
    }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;

private:
    void ensure_param_cache() const {
        if (param_cache_ready_) return;
        cached_param_names_.reserve(params_.size());
        for (const auto& p : params_) cached_param_names_.push_back(p->get_name()->get_name());
        cached_spec_length_ = 0;
        for (const auto& p : params_) {
            if (p->is_rest() || p->has_default()) break;
            cached_spec_length_++;
        }
        param_cache_ready_ = true;
    }
};

class ArrowFunctionExpression : public ASTNode {
private:
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<ASTNode> body_;
    bool is_async_;
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;
    // Built once on first evaluate(), reused by every instantiation -- same
    // idiom as FunctionExpression::cached_executable_. Only used by the
    // non-async branch (async arrows are a Function subclass, not yet
    // sharing an executable).
    mutable ExecutableRef<FunctionExecutable> cached_executable_;
    // Which parse tree this literal belongs to, recorded when the node was
    // built (see ScriptUnit::BuildScope). Null for trees built outside a unit,
    // which still take their own clone.
    ScriptUnit* owning_unit_ = ScriptUnit::building();

public:
    ArrowFunctionExpression(std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<ASTNode> body,
                           bool is_async,
                           const Position& start, const Position& end)
        : ASTNode(Type::ARROW_FUNCTION_EXPRESSION, start, end),
          params_(std::move(params)), body_(std::move(body)), is_async_(is_async) {}

    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    ASTNode* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_async() const { return is_async_; }
    bool has_block_body() const { return body_->get_type() == Type::BLOCK_STATEMENT; }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Drops the body subtree, leaving the range to read it back from. A body
    // holding inner literals reads back correctly too: an executable is found
    // by source offset rather than by node, and the literals inside are
    // stepped over rather than read again.
    void release_body() { body_.reset(); }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }
    // Keyed on where the body opens in the source, not on this node's address:
    // a body parsed back from the source is a different node for the same
    // declaration, and the executable has to be the same one either way. A
    // literal with no body range of its own keeps the node-local copy.
    const ExecutableRef<FunctionExecutable>& get_cached_executable() const {
        if (owning_unit_ && has_body_token_range()) {
            return owning_unit_->executable_at(body_src_first_);
        }
        return cached_executable_;
    }
    ScriptUnit* owning_unit() const { return owning_unit_; }
    void set_cached_executable(ExecutableRef<FunctionExecutable> exe) const {
        if (owning_unit_ && has_body_token_range()) {
            owning_unit_->set_executable_at(body_src_first_, std::move(exe));
            return;
        }
        cached_executable_ = std::move(exe);
    }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};


class AwaitExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    AwaitExpression(std::unique_ptr<ASTNode> argument, const Position& start, const Position& end)
        : ASTNode(Type::AWAIT_EXPRESSION, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class YieldExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;
    bool is_delegate_;

public:
    YieldExpression(std::unique_ptr<ASTNode> argument, bool is_delegate, const Position& start, const Position& end)
        : ASTNode(Type::YIELD_EXPRESSION, start, end), argument_(std::move(argument)), is_delegate_(is_delegate) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    bool is_delegate() const { return is_delegate_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};


class AsyncFunctionExpression : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;
    bool is_arrow_ = false;
    uint32_t src_start_ = 0;
    uint32_t src_end_ = 0;
    uint32_t body_tok_first_ = 0;
    // Where the body opens in the source. The executable cache is keyed on
    // this rather than on the token index: a body parsed back from the
    // source is lexed into its own sequence, so its token indices are not
    // the ones the first parse handed out, but its offsets are.
    uint32_t body_src_first_ = 0;
    uint32_t body_tok_last_ = 0;
    // Whether this body holds no nested function literal. Only a leaf is
    // released: releasing an outer body means re-parsing every body inside it
    // on the next call, and that was measured to cost more time than the
    // memory it returns is worth -- a trade this engine does not want.
    mutable int8_t body_strict_state_ = -1;
    mutable int8_t body_direct_eval_state_ = -1;
    bool is_decl_form_ = false; // `export default async function fn(){}`: HoistableDeclaration, not NamedEvaluation
    // Same cache-on-node pattern as FunctionExpression/FunctionDeclaration/
    // ArrowFunctionExpression's own cached_executable_.
    mutable ExecutableRef<FunctionExecutable> cached_executable_;
    // Which parse tree this literal belongs to, recorded when the node was
    // built (see ScriptUnit::BuildScope). Null for trees built outside a unit,
    // which still take their own clone.
    ScriptUnit* owning_unit_ = ScriptUnit::building();

public:
    // Keyed on where the body opens in the source, not on this node's address:
    // a body parsed back from the source is a different node for the same
    // declaration, and the executable has to be the same one either way. A
    // literal with no body range of its own keeps the node-local copy.
    const ExecutableRef<FunctionExecutable>& get_cached_executable() const {
        if (owning_unit_ && has_body_token_range()) {
            return owning_unit_->executable_at(body_src_first_);
        }
        return cached_executable_;
    }
    ScriptUnit* owning_unit() const { return owning_unit_; }
    void set_cached_executable(ExecutableRef<FunctionExecutable> exe) const {
        if (owning_unit_ && has_body_token_range()) {
            owning_unit_->set_executable_at(body_src_first_, std::move(exe));
            return;
        }
        cached_executable_ = std::move(exe);
    }
    AsyncFunctionExpression(std::unique_ptr<Identifier> id,
                           std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<BlockStatement> body,
                           const Position& start, const Position& end,
                           bool is_arrow = false)
        : ASTNode(Type::ASYNC_FUNCTION_EXPRESSION, start, end),
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)),
          is_arrow_(is_arrow) {}

    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_arrow() const { return is_arrow_; }
    // A range into the owning unit's source rather than a copy of it; see
    // ScriptUnit::source(). Materialized only when something actually asks,
    // which in practice is Function.prototype.toString.
    void set_source_range(size_t start, size_t end) {
        src_start_ = static_cast<uint32_t>(start);
        src_end_ = static_cast<uint32_t>(end);
    }
    std::string get_source_text() const {
        return owning_unit_ ? owning_unit_->source_range(src_start_, src_end_) : std::string();
    }
    bool has_source_range() const { return src_end_ > src_start_; }
    // Where this literal's body sits in the owning unit's token stream, so the
    // body can be parsed from there later instead of being kept as a tree.
    void set_body_token_range(size_t first, size_t last, uint32_t src_first) {
        body_tok_first_ = static_cast<uint32_t>(first);
        body_src_first_ = src_first;
        body_tok_last_ = static_cast<uint32_t>(last);
    }
    uint32_t body_token_first() const { return body_tok_first_; }
    uint32_t body_source_first() const { return body_src_first_; }
    uint32_t body_token_last() const { return body_tok_last_; }
    bool has_body_token_range() const { return body_tok_last_ > body_tok_first_; }
    // Drops the body subtree, leaving the range to read it back from. A body
    // holding inner literals reads back correctly too: an executable is found
    // by source offset rather than by node, and the literals inside are
    // stepped over rather than read again.
    void release_body() { body_.reset(); }
    // Facts about the body that instantiation needs. Cached on the literal
    // because they have to outlive the body itself: the tree is what gets
    // dropped, and asking the body again is exactly what is being avoided.
    // -1 not computed yet, 0 no, 1 yes.
    int8_t body_strict_state() const { return body_strict_state_; }
    int8_t body_direct_eval_state() const { return body_direct_eval_state_; }
    void set_body_facts(bool strict, bool direct_eval) const {
        body_strict_state_ = strict ? 1 : 0;
        body_direct_eval_state_ = direct_eval ? 1 : 0;
    }
    // A clone is built outside the parse that stamped the original, so it
    // carries the original's unit over explicitly or it has no source to
    // report.
    void set_source_ref(ScriptUnit* unit, uint32_t start, uint32_t end) {
        owning_unit_ = unit;
        src_start_ = start;
        src_end_ = end;
    }
    ScriptUnit* source_unit() const { return owning_unit_; }
    uint32_t source_start() const { return src_start_; }
    uint32_t source_end() const { return src_end_; }
    void set_decl_form(bool v) { is_decl_form_ = v; }
    bool is_decl_form() const { return is_decl_form_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ObjectLiteral : public ASTNode {
public:
    enum class PropertyType {
        Value,
        Method,
        Getter,
        Setter
    };

    struct Property {
        std::unique_ptr<ASTNode> key;
        std::unique_ptr<ASTNode> value;
        bool computed;
        bool shorthand = false;
        PropertyType type;

        Property(std::unique_ptr<ASTNode> k, std::unique_ptr<ASTNode> v, bool c = false, PropertyType t = PropertyType::Value)
            : key(std::move(k)), value(std::move(v)), computed(c), type(t) {}

        Property(std::unique_ptr<ASTNode> k, std::unique_ptr<ASTNode> v, bool c, bool m)
            : key(std::move(k)), value(std::move(v)), computed(c), type(m ? PropertyType::Method : PropertyType::Value) {}
    };

private:
    std::vector<std::unique_ptr<Property>> properties_;
    // `{...r,}` is a valid object literal but never a valid destructuring
    // pattern -- see ArrayLiteral for why the comma cannot be a property.
    bool trailing_comma_after_rest_ = false;

public:
    ObjectLiteral(std::vector<std::unique_ptr<Property>> properties,
                  const Position& start, const Position& end)
        : ASTNode(Type::OBJECT_LITERAL, start, end), properties_(std::move(properties)) {}
    
    const std::vector<std::unique_ptr<Property>>& get_properties() const { return properties_; }
    size_t property_count() const { return properties_.size(); }
    bool has_trailing_comma_after_rest() const { return trailing_comma_after_rest_; }
    void set_trailing_comma_after_rest(bool v) { trailing_comma_after_rest_ = v; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ArrayLiteral : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> elements_;
    // `[...a,]` is a valid array literal but never a valid destructuring
    // pattern. The comma adds no element, so it cannot be recorded as one:
    // a placeholder here would be indistinguishable from the hole in
    // `[...a,,]` and would show up in the literal's own length.
    bool trailing_comma_after_spread_ = false;

public:
    ArrayLiteral(std::vector<std::unique_ptr<ASTNode>> elements,
                 const Position& start, const Position& end)
        : ASTNode(Type::ARRAY_LITERAL, start, end), elements_(std::move(elements)) {}
    
    const std::vector<std::unique_ptr<ASTNode>>& get_elements() const { return elements_; }
    size_t element_count() const { return elements_.size(); }
    bool has_trailing_comma_after_spread() const { return trailing_comma_after_spread_; }
    void set_trailing_comma_after_spread(bool v) { trailing_comma_after_spread_ = v; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class SpreadElement : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    explicit SpreadElement(std::unique_ptr<ASTNode> argument, 
                          const Position& start, const Position& end)
        : ASTNode(Type::SPREAD_ELEMENT, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ReturnStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    explicit ReturnStatement(std::unique_ptr<ASTNode> argument, const Position& start, const Position& end)
        : ASTNode(Type::RETURN_STATEMENT, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    bool has_argument() const { return argument_ != nullptr; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class BreakStatement : public ASTNode {
private:
    std::string label_;

public:
    explicit BreakStatement(const Position& start, const Position& end, const std::string& label = "")
        : ASTNode(Type::BREAK_STATEMENT, start, end), label_(label) {}

    const std::string& get_label() const { return label_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ContinueStatement : public ASTNode {
private:
    std::string label_;

public:
    explicit ContinueStatement(const Position& start, const Position& end, const std::string& label = "")
        : ASTNode(Type::CONTINUE_STATEMENT, start, end), label_(label) {}

    const std::string& get_label() const { return label_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ExpressionStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    ExpressionStatement(std::unique_ptr<ASTNode> expression, const Position& start, const Position& end)
        : ASTNode(Type::EXPRESSION_STATEMENT, start, end), expression_(std::move(expression)) {}
    
    ASTNode* get_expression() const { return expression_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class EmptyStatement : public ASTNode {
public:
    EmptyStatement(const Position& start, const Position& end)
        : ASTNode(Type::EMPTY_STATEMENT, start, end) {}

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class LabeledStatement : public ASTNode {
private:
    std::string label_;
    std::unique_ptr<ASTNode> statement_;

public:
    LabeledStatement(const std::string& label, std::unique_ptr<ASTNode> statement,
                    const Position& start, const Position& end)
        : ASTNode(Type::LABELED_STATEMENT, start, end),
          label_(label), statement_(std::move(statement)) {}

    const std::string& get_label() const { return label_; }
    ASTNode* get_statement() const { return statement_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class TryStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> try_block_;
    std::unique_ptr<ASTNode> catch_clause_;
    std::unique_ptr<ASTNode> finally_block_;

public:
    TryStatement(std::unique_ptr<ASTNode> try_block, 
                std::unique_ptr<ASTNode> catch_clause,
                std::unique_ptr<ASTNode> finally_block,
                const Position& start, const Position& end)
        : ASTNode(Type::TRY_STATEMENT, start, end), 
          try_block_(std::move(try_block)),
          catch_clause_(std::move(catch_clause)),
          finally_block_(std::move(finally_block)) {}
    
    ASTNode* get_try_block() const { return try_block_.get(); }
    ASTNode* get_catch_clause() const { return catch_clause_.get(); }
    ASTNode* get_finally_block() const { return finally_block_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class CatchClause : public ASTNode {
private:
    std::string parameter_name_;
    std::unique_ptr<ASTNode> body_;
    std::unique_ptr<ASTNode> destructuring_pattern_;

public:
    CatchClause(const std::string& parameter_name,
               std::unique_ptr<ASTNode> body,
               const Position& start, const Position& end)
        : ASTNode(Type::CATCH_CLAUSE, start, end),
          parameter_name_(parameter_name),
          body_(std::move(body)) {}

    const std::string& get_parameter_name() const { return parameter_name_; }
    ASTNode* get_body() const { return body_.get(); }
    void set_destructuring_pattern(std::unique_ptr<ASTNode> p) { destructuring_pattern_ = std::move(p); }
    ASTNode* get_destructuring_pattern() const { return destructuring_pattern_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ThrowStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    ThrowStatement(std::unique_ptr<ASTNode> expression,
                  const Position& start, const Position& end)
        : ASTNode(Type::THROW_STATEMENT, start, end),
          expression_(std::move(expression)) {}
    
    ASTNode* get_expression() const { return expression_.get(); }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class SwitchStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> discriminant_;
    std::vector<std::unique_ptr<ASTNode>> cases_;

public:
    SwitchStatement(std::unique_ptr<ASTNode> discriminant,
                   std::vector<std::unique_ptr<ASTNode>> cases,
                   const Position& start, const Position& end)
        : ASTNode(Type::SWITCH_STATEMENT, start, end),
          discriminant_(std::move(discriminant)),
          cases_(std::move(cases)) {}
    
    ASTNode* get_discriminant() const { return discriminant_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_cases() const { return cases_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class CaseClause : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::vector<std::unique_ptr<ASTNode>> consequent_;

public:
    CaseClause(std::unique_ptr<ASTNode> test,
              std::vector<std::unique_ptr<ASTNode>> consequent,
              const Position& start, const Position& end)
        : ASTNode(Type::CASE_CLAUSE, start, end),
          test_(std::move(test)),
          consequent_(std::move(consequent)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_consequent() const { return consequent_; }
    bool is_default() const { return test_ == nullptr; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class Program : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> statements_;
    bool is_strict_ = false;
    bool may_suspend_ = false;
    bool hoisted_ = false;
    Value completion_promise_;

    void check_use_strict_directive(Context& ctx);
    void hoist_var_declarations(Context& ctx);
    void scan_for_var_declarations(ASTNode* node, Context& ctx);
    void hoist_lexical_declarations(Context& ctx);

public:
    Value evaluate(Context& ctx) override;

    // Everything InitializeEnvironment creates before a single statement runs:
    // vars, lexicals, function declarations. The module loader runs this before
    // resolving what the module imports, so a dependency calling back into this
    // module finds its functions already there. Idempotent.
    void hoist_declarations(Context& ctx);
    Program(std::vector<std::unique_ptr<ASTNode>> statements, const Position& start, const Position& end)
        : ASTNode(Type::PROGRAM, start, end), statements_(std::move(statements)) {}

    void set_strict(bool v) { is_strict_ = v; }
    bool is_strict() const { return is_strict_; }
    
    const std::vector<std::unique_ptr<ASTNode>>& get_statements() const { return statements_; }
    size_t statement_count() const { return statements_.size(); }

    // A module body may suspend on a top-level await instead of draining the
    // microtask queue where it stands. Set by the loader when nothing is
    // waiting on this module's evaluation; the promise it leaves behind is the
    // module's completion.
    void set_may_suspend(bool v) { may_suspend_ = v; }
    const Value& completion_promise() const { return completion_promise_; }
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ImportSpecifier : public ASTNode {
private:
    std::string imported_name_;
    std::string local_name_;

public:
    ImportSpecifier(const std::string& imported_name, const std::string& local_name,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_SPECIFIER, start, end),
          imported_name_(imported_name), local_name_(local_name) {}

    const std::string& get_imported_name() const { return imported_name_; }
    const std::string& get_local_name() const { return local_name_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ImportStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ImportSpecifier>> specifiers_;
    std::string module_source_;
    std::string namespace_alias_;
    std::string default_alias_;
    bool is_namespace_import_;
    bool is_default_import_;
    bool is_deferred_;
    std::string module_type_;

public:
    Value evaluate(Context& ctx) override;
    ImportStatement(std::vector<std::unique_ptr<ImportSpecifier>> specifiers,
                   const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), module_source_(module_source),
          is_namespace_import_(false), is_default_import_(false), is_deferred_(false) {}

    ImportStatement(const std::string& namespace_alias, const std::string& module_source,
                   const Position& start, const Position& end, bool is_deferred = false)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          module_source_(module_source), namespace_alias_(namespace_alias),
          is_namespace_import_(true), is_default_import_(false), is_deferred_(is_deferred) {}

    ImportStatement(const std::string& default_alias, const std::string& module_source,
                   bool is_default, const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          module_source_(module_source), default_alias_(default_alias),
          is_namespace_import_(false), is_default_import_(is_default), is_deferred_(false) {}

    ImportStatement(const std::string& default_alias,
                   std::vector<std::unique_ptr<ImportSpecifier>> specifiers,
                   const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), module_source_(module_source),
          default_alias_(default_alias),
          is_namespace_import_(false), is_default_import_(true), is_deferred_(false) {}

    // import x, * as ns from '...' -- default + namespace
    ImportStatement(const std::string& default_alias,
                   const std::string& namespace_alias,
                   const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          module_source_(module_source), namespace_alias_(namespace_alias),
          default_alias_(default_alias),
          is_namespace_import_(true), is_default_import_(true), is_deferred_(false) {}

    const std::vector<std::unique_ptr<ImportSpecifier>>& get_specifiers() const { return specifiers_; }
    const std::string& get_module_source() const { return module_source_; }
    const std::string& get_namespace_alias() const { return namespace_alias_; }
    const std::string& get_default_alias() const { return default_alias_; }
    bool is_namespace_import() const { return is_namespace_import_; }
    bool is_default_import() const { return is_default_import_; }
    bool is_mixed_import() const { return is_default_import_ && !specifiers_.empty(); }
    bool is_deferred() const { return is_deferred_; }

    // The `type` import attribute, if the clause named one. It decides what
    // kind of module the specifier resolves to.
    void set_module_type(const std::string& t) { module_type_ = t; }
    const std::string& get_module_type() const { return module_type_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ExportSpecifier : public ASTNode {
private:
    std::string local_name_;
    std::string exported_name_;

public:
    ExportSpecifier(const std::string& local_name, const std::string& exported_name,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_SPECIFIER, start, end),
          local_name_(local_name), exported_name_(exported_name) {}

    const std::string& get_local_name() const { return local_name_; }
    const std::string& get_exported_name() const { return exported_name_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class ExportStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ExportSpecifier>> specifiers_;
    std::unique_ptr<ASTNode> declaration_;
    std::unique_ptr<ASTNode> default_export_;
    std::string source_module_;
    bool is_default_export_;
    bool is_declaration_export_;
    bool is_re_export_;

public:
    Value evaluate(Context& ctx) override;
    ExportStatement(std::vector<std::unique_ptr<ExportSpecifier>> specifiers,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)),
          is_default_export_(false), is_declaration_export_(false), is_re_export_(false) {}

    ExportStatement(std::unique_ptr<ASTNode> declaration,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          declaration_(std::move(declaration)),
          is_default_export_(false), is_declaration_export_(true), is_re_export_(false) {}

    ExportStatement(std::unique_ptr<ASTNode> default_export, bool is_default,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          default_export_(std::move(default_export)),
          is_default_export_(is_default), is_declaration_export_(false), is_re_export_(false) {}

    ExportStatement(std::vector<std::unique_ptr<ExportSpecifier>> specifiers,
                   const std::string& source_module,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), source_module_(source_module),
          is_default_export_(false), is_declaration_export_(false), is_re_export_(true) {}

    const std::vector<std::unique_ptr<ExportSpecifier>>& get_specifiers() const { return specifiers_; }
    ASTNode* get_declaration() const { return declaration_.get(); }
    ASTNode* get_default_export() const { return default_export_.get(); }
    const std::string& get_source_module() const { return source_module_; }
    bool is_default_export() const { return is_default_export_; }
    bool is_declaration_export() const { return is_declaration_export_; }
    bool is_re_export() const { return is_re_export_; }

    // The export bookkeeping on its own. The compiler emits the wrapped
    // declaration itself and then asks for this, so the declaration is not
    // run a second time.
    Value link(Context& ctx, bool declaration_already_run);
    // `export default function () {}` is a HoistableDeclaration: its binding
    // holds the function before any statement of the module runs. Only the
    // function forms hoist -- a class, or any other expression, keeps its
    // dead zone.
    bool default_is_hoistable() const;
    void hoist_default(Context& ctx);
    
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class JSXElement : public ASTNode {
private:
    std::string tag_name_;
    std::vector<std::unique_ptr<ASTNode>> attributes_;
    std::vector<std::unique_ptr<ASTNode>> children_;
    bool self_closing_;

public:
    JSXElement(const std::string& tag_name,
               std::vector<std::unique_ptr<ASTNode>> attributes,
               std::vector<std::unique_ptr<ASTNode>> children,
               bool self_closing,
               const Position& start, const Position& end)
        : ASTNode(Type::JSX_ELEMENT, start, end),
          tag_name_(tag_name), attributes_(std::move(attributes)),
          children_(std::move(children)), self_closing_(self_closing) {}

    const std::string& get_tag_name() const { return tag_name_; }
    const std::vector<std::unique_ptr<ASTNode>>& get_attributes() const { return attributes_; }
    const std::vector<std::unique_ptr<ASTNode>>& get_children() const { return children_; }
    bool is_self_closing() const { return self_closing_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class JSXText : public ASTNode {
private:
    std::string text_;

public:
    JSXText(const std::string& text, const Position& start, const Position& end)
        : ASTNode(Type::JSX_TEXT, start, end), text_(text) {}

    const std::string& get_text() const { return text_; }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class JSXExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    JSXExpression(std::unique_ptr<ASTNode> expression, const Position& start, const Position& end)
        : ASTNode(Type::JSX_EXPRESSION, start, end), expression_(std::move(expression)) {}

    ASTNode* get_expression() const { return expression_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class JSXAttribute : public ASTNode {
private:
    std::string name_;
    std::unique_ptr<ASTNode> value_;

public:
    JSXAttribute(const std::string& name, std::unique_ptr<ASTNode> value,
                 const Position& start, const Position& end)
        : ASTNode(Type::JSX_ATTRIBUTE, start, end), name_(name), value_(std::move(value)) {}

    const std::string& get_name() const { return name_; }
    ASTNode* get_value() const { return value_.get(); }

    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

int get_loop_depth();
void increment_loop_depth();
void decrement_loop_depth();

}

#endif
