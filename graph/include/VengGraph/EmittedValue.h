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

    /// @brief Kind of a generated MaterialParams field, distinguishing the .vmat row it produces.
    enum class EmittedFieldKind : Veng::u8
    {
        /// @brief A bindless sampled-image handle slot (`uint`); its .vmat row is a texture.
        TextureHandle,
        /// @brief A bindless sampler handle slot (`uint`); its .vmat row is a sampler.
        SamplerHandle,
        /// @brief An exposed or engine-bound scalar/vector param; its .vmat row is a float/vecN.
        Param,
    };

    /// @brief One generated MaterialParams field an emit-fn contributes.
    ///
    /// The generated parameter block is exactly the texture + exposed/engine-bound param
    /// nodes: a TextureSample contributes its texture and sampler handle slots, an exposed
    /// or engine-bound Param its value field (a const Param folds its value inline and
    /// contributes nothing). The emit walk collects these, orders them large-alignment-first
    /// (so the cooker's std140 reflection and the shader's scalar-layout Load resolve identical
    /// offsets), and emits the final struct + the matching .vmat field list from the one set.
    struct EmittedParamField
    {
        /// @brief The Slang member name (a node-unique identifier).
        Veng::string Name;
        /// @brief The Slang member type ("uint", "float", "float2".."float4").
        Veng::string SlangType;
        /// @brief Which .vmat row kind this field produces.
        EmittedFieldKind Kind = EmittedFieldKind::Param;
        /// @brief std140/scalar alignment of the member in bytes (16 vec3/vec4, 8 vec2, 4 scalar/uint).
        ///
        /// The walk orders fields by descending alignment so std140 reflection and scalar
        /// Load\<MaterialParams\> resolve identical offsets.
        Veng::u32 Alignment = 4;
        /// @brief Component count: 1 scalar/uint, 2/3/4 for a vecN param.
        Veng::u32 ComponentCount = 1;
        /// @brief True for a uint handle slot; false for a float param.
        bool IsUint = false;
        /// @brief Authored default components (exposed param); empty for a handle or engine-bound field.
        Veng::vector<Veng::f32> Default;
        /// @brief Texture AssetId for a TextureHandle field; 0 for a runtime/engine-bound handle.
        Veng::u64 TextureId = 0;
        /// @brief For a SamplerHandle field, the name of the paired TextureHandle field.
        Veng::string SamplerTexture;
    };

    /// @brief Mutable state threaded through the emit walk.
    ///
    /// Owns the SSA-temp counter, the growing function body the walk appends each
    /// node's temp declarations to, the stable key of the node currently emitting
    /// (so an emit-fn names node-unique param fields deterministically), and the
    /// collected MaterialParams fields.
    struct EmitContext
    {
        /// @brief The generated function-body lines, one SSA temp declaration each.
        Veng::string Body;
        /// @brief Monotonic counter backing unique temp names.
        Veng::u32 TempCounter = 0;
        /// @brief Stable identifier of the node currently emitting (set by the walk).
        Veng::string NodeKey;
        /// @brief MaterialParams fields the emit-fns contributed, in walk order.
        Veng::vector<EmittedParamField> ParamFields;
    };

    /// @brief A node type's emit-fn: input values + property bytes → one value per output pin.
    ///
    /// Registered per node type by the material catalog. @p inputs is one EmittedValue
    /// per input pin (the pin's default when the input is unconnected); @p propertyBytes
    /// is the node instance's reflected property buffer; @p ctx threads the temp counter
    /// and the growing body. The returned vector carries one EmittedValue per output pin,
    /// in pin order — one element for a single-output node (Multiply, Param), N for a
    /// fan-out node (Split's per-channel scalars).
    using NodeEmitFn = Veng::function<Veng::vector<EmittedValue>(
        std::span<const EmittedValue> inputs, std::span<const std::byte> propertyBytes,
        EmitContext& ctx)>;
}
