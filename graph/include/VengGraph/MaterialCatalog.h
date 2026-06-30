#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Material.h>
#include <Veng/Reflection/Reflect.h>

#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeType.h>
#include <VengGraph/EmittedValue.h>

#include <unordered_map>

namespace Veng
{
    class TypeRegistry;
}

namespace VengGraph
{
    /// @brief How a Param node's value reaches the generated shader.
    ///
    /// One Param node with a provenance flag, not three node types. The provenance
    /// decides whether the value folds inline, becomes an author-tweakable uniform, or
    /// becomes a uniform the engine writes by name at runtime.
    enum class ParamProvenance : Veng::i32
    {
        /// @brief Folds the authored value inline as a Slang literal; contributes no field.
        Const = 0,
        /// @brief A generated MaterialParams field with the authored default; the instance override surface.
        Exposed = 1,
        /// @brief A generated MaterialParams field the engine writes by name at runtime; no authored default, not instance-overridable.
        EngineBound = 2,
    };

    /// @brief A node's authored leaf type: the Slang scalar/vector kind it emits.
    ///
    /// Recorded explicitly on a Constant node's property POD so the emitted struct field
    /// type and any inline literal are unambiguous, rather than inferred from a value
    /// blob's size.
    enum class MaterialLeafType : Veng::i32
    {
        /// @brief A single-component float (`float`).
        Float = 0,
        /// @brief A two-component float vector (`float2`).
        Vec2 = 1,
        /// @brief A three-component float vector (`float3`).
        Vec3 = 2,
        /// @brief A four-component float vector (`float4`).
        Vec4 = 3,
        /// @brief A single-component unsigned integer (`uint`).
        Uint = 4,
    };

    /// @brief Catalog ids of the base material node types, resolved by stable name.
    ///
    /// The math/swizzle/utility nodes RegisterMaterialNodeTypes adds beside these are
    /// resolved by name from the catalog rather than carried here; only the three the
    /// emit walk and the editor reference structurally are named.
    struct MaterialNodeTypes
    {
        /// @brief Catalog id of the TextureSample node type.
        NodeTypeId TextureSample;
        /// @brief Catalog id of the Param node type.
        NodeTypeId Param;
        /// @brief Catalog id of the MaterialOutput node type.
        NodeTypeId MaterialOutput;
    };

    /// @brief Emit-fn table keyed by node-type id.
    ///
    /// The topology core's NodeType is data, not a subclass, so the emit-fn lives on
    /// the material-catalog side: RegisterMaterialNodeTypes fills this beside minting
    /// the node types, and CompileMaterialGraph resolves a node's emit-fn through it.
    struct MaterialEmitTable
    {
        /// @brief NodeTypeId value → its emit-fn.
        std::unordered_map<Veng::u32, NodeEmitFn> Emitters;

        /// @brief Finds a node type's emit-fn, or nullptr when none is registered.
        [[nodiscard]] const NodeEmitFn* Find(NodeTypeId id) const;
    };

    /// @brief Stable serialized name of the TextureSample node type.
    inline constexpr const char* TextureSampleTypeName = "TextureSample";
    /// @brief Stable serialized name of the Param node type.
    inline constexpr const char* ParamTypeName = "Param";
    /// @brief Stable serialized name of the MaterialOutput node type.
    inline constexpr const char* MaterialOutputTypeName = "MaterialOutput";

    /// @brief Output color pin name on a TextureSample node.
    inline constexpr const char* TextureSampleColorPin = "Color";
    /// @brief Optional UV input pin name on a TextureSample node.
    inline constexpr const char* TextureSampleUVPin = "UV";
    /// @brief Texture asset-handle property name on a TextureSample node.
    inline constexpr const char* TextureSampleTextureProperty = "Texture";

    /// @brief Output value pin name on a Param node.
    inline constexpr const char* ParamValuePin = "Value";
    /// @brief Value property name on a Param node.
    inline constexpr const char* ParamValueProperty = "Value";
    /// @brief Provenance property name on a Param node.
    inline constexpr const char* ParamProvenanceProperty = "Provenance";
    /// @brief Authored field-name property name on a Param, ScalarParam, or TextureSample node.
    ///
    /// When non-empty, the generated MaterialParams field takes this exact name (so an
    /// engine-bound field the engine writes by name resolves); empty falls back to the
    /// walk's node key.
    inline constexpr const char* NodeNameProperty = "Name";

    /// @brief Stable serialized name of the Constant node type.
    inline constexpr const char* ConstantTypeName = "Constant";
    /// @brief Stable serialized name of the ScalarParam node type.
    inline constexpr const char* ScalarParamTypeName = "ScalarParam";
    /// @brief Stable serialized name of the Multiply node type.
    inline constexpr const char* MultiplyTypeName = "Multiply";
    /// @brief Stable serialized name of the Add node type.
    inline constexpr const char* AddTypeName = "Add";
    /// @brief Stable serialized name of the Subtract node type.
    inline constexpr const char* SubtractTypeName = "Subtract";
    /// @brief Stable serialized name of the Divide node type.
    inline constexpr const char* DivideTypeName = "Divide";
    /// @brief Stable serialized name of the Lerp node type.
    inline constexpr const char* LerpTypeName = "Lerp";
    /// @brief Stable serialized name of the Saturate node type.
    inline constexpr const char* SaturateTypeName = "Saturate";
    /// @brief Stable serialized name of the Clamp node type.
    inline constexpr const char* ClampTypeName = "Clamp";
    /// @brief Stable serialized name of the OneMinus node type.
    inline constexpr const char* OneMinusTypeName = "OneMinus";
    /// @brief Stable serialized name of the Dot node type.
    inline constexpr const char* DotTypeName = "Dot";
    /// @brief Stable serialized name of the Cross node type.
    inline constexpr const char* CrossTypeName = "Cross";
    /// @brief Stable serialized name of the Normalize node type.
    inline constexpr const char* NormalizeTypeName = "Normalize";
    /// @brief Stable serialized name of the Length node type.
    inline constexpr const char* LengthTypeName = "Length";
    /// @brief Stable serialized name of the Min node type.
    inline constexpr const char* MinTypeName = "Min";
    /// @brief Stable serialized name of the Max node type.
    inline constexpr const char* MaxTypeName = "Max";
    /// @brief Stable serialized name of the ScreenUV node type.
    inline constexpr const char* ScreenUVTypeName = "ScreenUV";
    /// @brief Stable serialized name of the Split node type.
    inline constexpr const char* SplitTypeName = "Split";
    /// @brief Stable serialized name of the Combine node type.
    inline constexpr const char* CombineTypeName = "Combine";

    /// @brief Authored value property name on a Constant node.
    inline constexpr const char* ConstantValueProperty = "Value";
    /// @brief Leaf-type property name on a Constant node.
    inline constexpr const char* ConstantLeafTypeProperty = "LeafType";

    /// @brief Default value property name on a ScalarParam node.
    inline constexpr const char* ScalarParamValueProperty = "Value";
    /// @brief Provenance property name on a ScalarParam node.
    inline constexpr const char* ScalarParamProvenanceProperty = "Provenance";

    /// @brief MaterialOutput sink pin name for the Surface domain albedo channel.
    inline constexpr const char* OutputAlbedoPin = "Albedo";
    /// @brief MaterialOutput sink pin name for the Surface domain normal channel.
    inline constexpr const char* OutputNormalPin = "Normal";
    /// @brief MaterialOutput sink pin name for the PostProcess domain color channel.
    inline constexpr const char* OutputColorPin = "Color";

    /// @brief One sink pin of a domain's MaterialOutput contract: its name and leaf type.
    struct DomainOutputPin
    {
        /// @brief Pin name (matches the output constant above for the domain).
        Veng::string Name;
        /// @brief Pin type (Value kind, with the appropriate leaf TypeId).
        PinType Type;
    };

    /// @brief Returns the fixed output-contract sink pins for a material domain.
    ///
    /// Surface yields Albedo (vec4) + Normal (vec3), matching GBufferOutput.
    /// PostProcess yields a single Color (vec4).
    [[nodiscard]] Veng::vector<DomainOutputPin> DomainOutputContract(Veng::MaterialDomain domain);

    /// @brief Registers the fixed, schema-independent material node types into @p catalog.
    ///
    /// The node set is a fixed function of the domain only, shaped by pin leaf types —
    /// not by any loaded shader's reflected fields: TextureSample always has a UV input
    /// and a color output, Param is sized by its own property, MaterialOutput's sinks
    /// come from the domain contract. Beside minting the types it fills @p emit with one
    /// emit-fn per type. The catalog and emit table must outlive any graph built against
    /// them.
    /// @param catalog Target catalog; mutated by registration.
    /// @param emit    Target emit table; one entry added per node type.
    /// @param domain  Material domain; shapes the MaterialOutput sinks and entry point.
    /// @return The minted node-type ids.
    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog, MaterialEmitTable& emit,
                                                Veng::MaterialDomain domain);

    /// @brief Registers the shading-math, swizzle, and utility node types into @p catalog.
    ///
    /// The expression-emitter set: Constant and ScalarParam (sources), Multiply / Add /
    /// Subtract / Divide (arithmetic), Lerp / Saturate / Clamp / OneMinus (shaping), Dot /
    /// Cross / Normalize / Length (vector algebra), and Split / Combine (channel plumbing).
    /// Each is domain-independent (math is math), so this is called for every domain.
    /// Beside minting the types it fills @p emit with one emit-fn per type.
    /// @param catalog Target catalog; mutated by registration.
    /// @param emit    Target emit table; one entry added per node type.
    void RegisterMathNodeTypes(NodeCatalog& catalog, MaterialEmitTable& emit);

    /// @brief Registers the reflected enum types carried by material node properties.
    ///
    /// A node's enum property (a Param's ParamProvenance, a Constant's MaterialLeafType)
    /// is drawn by the editor's reflection inspector, which resolves the enumerator names
    /// through the host TypeRegistry; an unregistered enum id is a fatal lookup. The catalog
    /// itself is registry-free (it reads FieldClass/TypeId directly), so a host that draws
    /// node properties registers these once alongside the engine builtins.
    /// @param registry Target registry; the enum types are registered idempotently.
    void RegisterMaterialGraphTypes(Veng::TypeRegistry& registry);

    /// @brief Connection predicate for material graphs: exact TypeId equality plus
    /// f32→vecN splat and vec4→vec3/vec2 truncation.
    ///
    /// Coercion lives here only; the topology core stays a pure DAG enforcer.
    [[nodiscard]] bool MaterialCanConnect(const PinType& from, const PinType& to);

    /// @brief Wraps an upstream expression to coerce its value into a target pin type.
    ///
    /// Applies the same coercions MaterialCanConnect permits: f32→vecN splat (`v.xxx`),
    /// vec4→vec3/vec2 truncation (`v.xyz` / `v.xy`). An exact-type or unrecognised pair
    /// returns the expression unchanged. EmittedValue::Type makes this a typed
    /// operation, not a guess at the text.
    /// @param expr Source Slang expression.
    /// @param from Source leaf type.
    /// @param to   Destination leaf type.
    /// @return The coerced expression.
    [[nodiscard]] Veng::string CoerceExpr(const Veng::string& expr, const PinType& from,
                                          const PinType& to);
}

VE_ENUM(::VengGraph::ParamProvenance, 0x9F3C71A4D8E2065BULL)
VE_ENUMERATOR(Const)
VE_ENUMERATOR(Exposed)
VE_ENUMERATOR(EngineBound)
VE_ENUM_END();

VE_ENUM(::VengGraph::MaterialLeafType, 0x5AC3779D52EF1380ULL)
VE_ENUMERATOR(Float)
VE_ENUMERATOR(Vec2)
VE_ENUMERATOR(Vec3)
VE_ENUMERATOR(Vec4)
VE_ENUMERATOR(Uint)
VE_ENUM_END();
