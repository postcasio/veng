#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeType.h>

#include <VengGraph/EmittedValue.h>
#include <VengGraph/MaterialShaderInterface.h>
#include <VengGraph/MaterialCatalog.h>

namespace VengGraph
{
    /// @brief A generated material fragment: the Slang source and its domain.
    ///
    /// CompileMaterialGraph's product. The param-schema and field-list halves of a
    /// full cook are filled in by a later plan; this carries the compilable source and
    /// the domain it was generated for.
    struct GeneratedFragment
    {
        /// @brief The generated Slang fragment-shader source.
        Veng::string Source;
        /// @brief The material domain the entry point was generated for.
        Veng::MaterialDomain Domain = Veng::MaterialDomain::Surface;
    };

    /// @brief Walks a material node graph into generated Slang fragment source.
    ///
    /// Topologically walks the graph from MaterialOutput (the topology core guarantees
    /// acyclicity, so an unreached node never emits — free dead-code elimination). For
    /// each node it gathers its inputs' EmittedValues (an unconnected input yields the
    /// pin's default), calls the node type's emit-fn, and assigns one SSA temp per
    /// output pin that downstream inputs substitute. A value used more than once is a
    /// temp (a shared TextureSample samples once); a single-use value inlines, so the
    /// output is a pure function of the graph. Link-recorded coercion (f32→vecN,
    /// vec4→vec3/vec2) wraps an upstream expr substituted into a lower-arity input.
    /// MaterialOutput emits the domain entry point (GBufferOutput for Surface,
    /// SV_Target0 for PostProcess), defaulting unconnected sinks, prefixed with
    /// #include "Veng/material.slang" and a provisional MaterialParams struct.
    /// @param graph   The material graph to walk.
    /// @param catalog Catalog resolving node types by id.
    /// @param emit    Emit-fn table keyed by node-type id (from RegisterMaterialNodeTypes).
    /// @param domain  The material domain whose entry point MaterialOutput emits.
    /// @return Error when the graph has no MaterialOutput node, more than one, or a
    ///         node type lacks an emit-fn.
    [[nodiscard]] Veng::Result<GeneratedFragment>
    CompileMaterialGraph(const NodeGraph& graph, const NodeCatalog& catalog,
                         const MaterialEmitTable& emit, Veng::MaterialDomain domain);

    /// @brief One entry of a compiled .vmat "fields" array.
    ///
    /// @p Type selects which payload member is meaningful. nlohmann::json stays
    /// out of this header; serialization lives in the .cpp.
    struct CompiledField
    {
        /// @brief Field name matching the shader's reflected field table.
        Veng::string Name;
        /// @brief Field type tag: "texture" | "sampler" | "float" | "vec2".."vec4" | "uint".
        Veng::string Type;
        /// @brief Texture AssetId (meaningful when Type == "texture").
        Veng::u64 TextureId = 0;
        /// @brief Paired texture field name (meaningful when Type == "sampler").
        Veng::string SamplerTexture;
        /// @brief Component values (meaningful when Type is "float" or "vecN").
        Veng::vector<Veng::f32> Values;
        /// @brief Integer value (meaningful when Type == "uint").
        Veng::u32 UintValue = 0;
    };

    /// @brief Serializes a compiled field list into a .vmat JSON document string.
    ///
    /// Writes the lowercase "domain" key, the "shaders" block (vertex/fragment ids
    /// from @p shader), and the regenerated "fields" array. JSON assembly is in the
    /// .cpp so this header carries no JSON type.
    [[nodiscard]] Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                                 const MaterialShaderInterface& shader,
                                                 Veng::MaterialDomain domain);
}
