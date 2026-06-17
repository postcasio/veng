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

    // Walks TopoOrder from the MaterialOutput node's connected inputs and emits
    // one CompiledField per connected output field:
    //   - a texture input fed by a TextureSample → a "texture" field (the node's
    //     Texture property AssetId) plus an implicit paired "sampler" field;
    //   - a param input fed by a Param → a "float"/"vecN"/"uint" field, applying
    //     the recorded coercion (f32→vecN splat, vec4→vec3/vec2 truncate);
    //   - an unconnected input → omitted (the importer's schema tolerance keeps
    //     its default).
    // Returns an error when the graph has no MaterialOutput node or an input is
    // fed by an unsupported node.
    [[nodiscard]] Veng::Result<Veng::vector<CompiledField>> CompileMaterialGraph(
        const NodeGraph& graph, const NodeCatalog& catalog,
        const MaterialShaderInterface& shader);

    // Serializes a compiled field list into a .vmat document string: the "shaders"
    // block (vertex/fragment ids from shader) plus the regenerated "fields" array.
    // The JSON assembly lives in the .cpp so this header carries no JSON type.
    [[nodiscard]] Veng::string WriteMaterialVmat(const Veng::vector<CompiledField>& fields,
                                                 const MaterialShaderInterface& shader);

    // Synthesizes a default graph from a flat material's field table: a
    // MaterialOutput, a TextureSample per texture field (its Texture property set
    // to the field's TextureId) feeding the matching output pin, a Param per param
    // field feeding its pin — laid out left-to-right. A sampler field is consumed
    // by its paired texture, never its own node. Import does not rewrite fields;
    // CompileMaterialGraph(BuildGraphFromMaterial(shader)) reproduces the source
    // field list (the round-trip identity guard).
    [[nodiscard]] NodeGraph BuildGraphFromMaterial(const MaterialShaderInterface& shader,
                                                   const NodeCatalog& catalog,
                                                   const MaterialNodeTypes& types);
}
