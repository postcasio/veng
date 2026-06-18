#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Material.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialShaderInterface.h"

namespace VengEditor
{
    // The catalog id of each v1 material node type, resolved off a built catalog
    // by stable name (TextureSample / Param / MaterialOutput). The MaterialOutput
    // type's pins are the loaded material's domain output contract, so the catalog
    // is built per loaded material with its domain.
    struct MaterialNodeTypes
    {
        NodeTypeId TextureSample;
        NodeTypeId Param;
        NodeTypeId MaterialOutput;
    };

    // The stable serialized names of the v1 material node types.
    inline constexpr const char* TextureSampleTypeName = "TextureSample";
    inline constexpr const char* ParamTypeName = "Param";
    inline constexpr const char* MaterialOutputTypeName = "MaterialOutput";

    // The output pin name a TextureSample feeds and the optional UV input.
    inline constexpr const char* TextureSampleColorPin = "Color";
    inline constexpr const char* TextureSampleUVPin = "UV";
    inline constexpr const char* TextureSampleTextureProperty = "Texture";

    // The Param node's single typed output pin and its value property.
    inline constexpr const char* ParamValuePin = "Value";
    inline constexpr const char* ParamValueProperty = "Value";

    // The MaterialOutput sink pin names per domain. Surface's sinks express the
    // fixed GBufferOutput contract (Albedo + Normal); PostProcess's sink is the
    // single final Color. These are the domain's output contract, not the loaded
    // shader's reflected fields.
    inline constexpr const char* OutputAlbedoPin = "Albedo";
    inline constexpr const char* OutputNormalPin = "Normal";
    inline constexpr const char* OutputColorPin = "Color";

    // One sink pin of a domain's MaterialOutput contract: its name and leaf type.
    struct DomainOutputPin
    {
        Veng::string Name;
        PinType Type;
    };

    // The fixed output-contract sink pins of a material domain. Surface yields the
    // g-buffer pins (Albedo vec4 + Normal vec3, matching GBufferOutput);
    // PostProcess yields the single Color pin (vec4).
    [[nodiscard]] Veng::vector<DomainOutputPin> DomainOutputContract(Veng::MaterialDomain domain);

    // Registers the three v1 material node types into catalog. TextureSample and
    // Param (the input-side catalog) come from the shader interface — which fields
    // exist and a Param's pin sizing. MaterialOutput's pins are the domain's output
    // contract, independent of the loaded shader's fields. Returns the minted type
    // ids. The catalog must outlive any graph built against it.
    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog,
                                                const MaterialShaderInterface& shader,
                                                Veng::MaterialDomain domain);

    // The coercion-aware connection predicate the material catalog supplies to a
    // NodeGraph: exact-TypeId equality plus f32→vecN splat and vec4→vec3/vec2
    // truncate. This is the one place coercion lives; the topology core stays a
    // pure DAG enforcer.
    [[nodiscard]] bool MaterialCanConnect(const PinType& from, const PinType& to);
}
