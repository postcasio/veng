#pragma once

#include <Veng/Veng.h>

#include <VengGraph/NodeGraph.h>

#include <span>

namespace VengGraph
{
    /// @brief A thin typed code-chunk threaded by the emit walk.
    ///
    /// The single intermediate between the graph (the AST) and the generated Slang
    /// text: a node's emit-fn maps its input EmittedValues to an output EmittedValue.
    /// It is not a parsed expression tree — the graph is already the typed acyclic
    /// DAG — only the chunk model (Unreal's translator chunks, Unity's slot vars) that
    /// makes coercion and temp-vs-inline first-class. @ref Type carries the leaf type
    /// so coercion composes by type, not by string-munging the expression.
    struct EmittedValue
    {
        /// @brief Slang expression: an SSA temp ("n2_Color") or an inlined literal
        ///        ("float4(0.8,0.2,0.1,1)").
        Veng::string Expr;
        /// @brief Leaf type of the value; coercion is applied against it.
        PinType Type;
        /// @brief True when the expression is a self-contained constant the walk may
        ///        fold inline rather than read from a temp.
        bool IsConst = false;
    };

    /// @brief One provisional MaterialParams field an emit-fn contributes.
    ///
    /// The generated parameter block is implied by the texture + param nodes: a
    /// TextureSample contributes its texture and sampler handle slots, a Param its
    /// value field. The emit walk collects these and emits a provisional struct so the
    /// generated source is compilable; a later plan materializes the final struct.
    struct EmittedParamField
    {
        /// @brief The Slang member name (a node-unique identifier).
        Veng::string Name;
        /// @brief The Slang member type ("uint", "float", "float4", …).
        Veng::string SlangType;
    };

    /// @brief Mutable state threaded through the emit walk.
    ///
    /// Owns the SSA-temp counter, the growing function body the walk appends each
    /// node's temp declarations to, the stable key of the node currently emitting
    /// (so an emit-fn names node-unique param fields deterministically), and the
    /// collected provisional MaterialParams fields.
    struct EmitContext
    {
        /// @brief The generated function-body lines, one SSA temp declaration each.
        Veng::string Body;
        /// @brief Monotonic counter backing unique temp names.
        Veng::u32 TempCounter = 0;
        /// @brief Stable identifier of the node currently emitting (set by the walk).
        Veng::string NodeKey;
        /// @brief Provisional MaterialParams fields the emit-fns contributed.
        Veng::vector<EmittedParamField> ParamFields;
    };

    /// @brief A node type's emit-fn: input values + property bytes → an output value.
    ///
    /// Registered per node type by the material catalog. @p inputs is one EmittedValue
    /// per input pin (the pin's default when the input is unconnected); @p propertyBytes
    /// is the node instance's reflected property buffer; @p ctx threads the temp counter
    /// and the growing body. A multi-output node type is not yet expressed — each known
    /// type has at most one output, so the fn returns a single value.
    using NodeEmitFn =
        Veng::function<EmittedValue(std::span<const EmittedValue> inputs,
                                    std::span<const std::byte> propertyBytes, EmitContext& ctx)>;
}
