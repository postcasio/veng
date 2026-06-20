#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/Material.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialShaderInterface.h"

namespace VengEditor
{
    /// @brief Catalog ids of the three material node types, resolved by stable name.
    ///
    /// The MaterialOutput type's pins reflect the domain's output contract, so the
    /// catalog is rebuilt per loaded material with its domain.
    struct MaterialNodeTypes
    {
        /// @brief Catalog id of the TextureSample node type.
        NodeTypeId TextureSample;
        /// @brief Catalog id of the Param node type.
        NodeTypeId Param;
        /// @brief Catalog id of the MaterialOutput node type.
        NodeTypeId MaterialOutput;
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

    /// @brief Registers the three material node types into @p catalog and returns their ids.
    ///
    /// TextureSample and Param are built from the shader interface (which fields exist,
    /// Param pin sizing). MaterialOutput's pins come from the domain's output contract,
    /// independent of the loaded shader's fields. The catalog must outlive any graph
    /// built against it.
    /// @param catalog  Target catalog; mutated by registration.
    /// @param shader   Loaded material's field table and shader ids.
    /// @param domain   Material domain; shapes the MaterialOutput sinks.
    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog,
                                                const MaterialShaderInterface& shader,
                                                Veng::MaterialDomain domain);

    /// @brief Connection predicate for material graphs: exact TypeId equality plus
    /// f32→vecN splat and vec4→vec3/vec2 truncation.
    ///
    /// Coercion lives here only; the topology core stays a pure DAG enforcer.
    [[nodiscard]] bool MaterialCanConnect(const PinType& from, const PinType& to);
}
