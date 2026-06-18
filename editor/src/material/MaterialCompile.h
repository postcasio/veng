#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialShaderInterface.h"
#include "material/MaterialCatalog.h"

namespace VengEditor
{
    // One entry of a compiled .vmat "fields" array — the typed form the .cpp
    // serializes to JSON (nlohmann::json stays out of this header). Type selects
    // which payload member is meaningful.
    struct CompiledField
    {
        Veng::string Name;
        Veng::string Type;            // "texture" | "sampler" | "float" | "vec2".."vec4" | "uint"
        Veng::u64 TextureId = 0;      // texture: the AssetId
        Veng::string SamplerTexture;  // sampler: the paired texture field's name
        Veng::vector<Veng::f32> Values; // float / vecN: the components
        Veng::u32 UintValue = 0;      // uint
    };

    // Emits the bound .vmat field list — one CompiledField per the loaded
    // material's reflected field — sourcing values from the graph's upstream
    // feeders (a TextureSample per texture handle, a Param per scalar/vector param),
    // consumed in node-creation order:
    //   - a texture field → a "texture" field (the next TextureSample's Texture
    //     property AssetId) plus an implicit paired "sampler" field;
    //   - a param field → a "float"/"vecN" field (the next Param's value, coerced
    //     to the field's arity);
    //   - a sampler field → a "sampler" field paired by the <Texture>Sampler
    //     convention.
    // The domain shapes the output node's sink pins (Albedo/Normal for Surface,
    // Color for PostProcess), not the bound field set. Returns an error when the
    // graph has no MaterialOutput node or a param field has an unsupported size.
    [[nodiscard]] Veng::Result<Veng::vector<CompiledField>> CompileMaterialGraph(
        const NodeGraph& graph, const NodeCatalog& catalog,
        const MaterialShaderInterface& shader, Veng::MaterialDomain domain);

    // Serializes a compiled field list into a .vmat document string: the lowercase
    // "domain" key (surface/postprocess), the "shaders" block (vertex/fragment ids
    // from shader), and the regenerated "fields" array. The JSON assembly lives in
    // the .cpp so this header carries no JSON type.
    [[nodiscard]] Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                                 const MaterialShaderInterface& shader,
                                                 Veng::MaterialDomain domain);

    // Synthesizes a default graph from a flat material's field table: a
    // MaterialOutput seeded from the domain output contract (its sink pins), a
    // TextureSample per texture field (its Texture property set to the field's
    // TextureId), a Param per param field, each wired into the next free
    // type-compatible domain sink — laid out left-to-right. A sampler field is
    // consumed by its paired texture, never its own node. Import does not rewrite
    // fields; CompileMaterialGraph(BuildGraphFromMaterial(shader)) reproduces the
    // source field list (the round-trip identity guard).
    [[nodiscard]] NodeGraph BuildGraphFromMaterial(const MaterialShaderInterface& shader,
                                                   const NodeCatalog& catalog,
                                                   const MaterialNodeTypes& types);
}
