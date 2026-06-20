#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialShaderInterface.h"
#include "material/MaterialCatalog.h"

namespace VengEditor
{
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

    /// @brief Compiles a material node graph into the .vmat field list.
    ///
    /// Emits one CompiledField per the loaded material's reflected field, sourcing
    /// values from the graph's upstream feeders consumed in node-creation order:
    /// a texture field → "texture" entry (next TextureSample's AssetId) plus an
    /// implicit paired "sampler"; a param field → "float"/"vecN" entry (next Param's
    /// value coerced to the field's arity); a sampler field → "sampler" entry paired
    /// by the \<Texture\>Sampler convention.
    ///
    /// The domain shapes the MaterialOutput's sink pins, not the bound field set.
    /// @return Error when the graph has no MaterialOutput node or a param field has
    ///         an unsupported size.
    [[nodiscard]] Veng::Result<Veng::vector<CompiledField>> CompileMaterialGraph(
        const NodeGraph& graph, const NodeCatalog& catalog,
        const MaterialShaderInterface& shader, Veng::MaterialDomain domain);

    /// @brief Serializes a compiled field list into a .vmat JSON document string.
    ///
    /// Writes the lowercase "domain" key, the "shaders" block (vertex/fragment ids
    /// from @p shader), and the regenerated "fields" array. JSON assembly is in the
    /// .cpp so this header carries no JSON type.
    [[nodiscard]] Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                                 const MaterialShaderInterface& shader,
                                                 Veng::MaterialDomain domain);

    /// @brief Synthesizes a default node graph from a material's field table.
    ///
    /// Produces a MaterialOutput seeded from the domain output contract, a
    /// TextureSample per texture field (Texture property set to the field's
    /// TextureId), and a Param per param field, each wired into the next free
    /// type-compatible domain sink. Sampler fields are consumed by their paired
    /// texture node, not their own node. CompileMaterialGraph applied to the result
    /// reproduces the source field list exactly.
    [[nodiscard]] NodeGraph BuildGraphFromMaterial(const MaterialShaderInterface& shader,
                                                   const NodeCatalog& catalog,
                                                   const MaterialNodeTypes& types);
}
