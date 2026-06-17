#pragma once

#include <Veng/Veng.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialShaderInterface.h"

namespace VengEditor
{
    // The catalog id of each v1 material node type, resolved off a built catalog
    // by stable name (TextureSample / Param / MaterialOutput). The MaterialOutput
    // type's pins are derived from the shader interface's field table, so the
    // catalog is built per loaded material.
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

    // Registers the three v1 material node types into catalog, deriving the
    // MaterialOutput node's input pins from shader.Fields by field Kind (a param
    // field → one typed input pin, a texture-handle field → one input pin, a
    // sampler-handle field → no pin). Returns the minted type ids. The catalog
    // must outlive any graph built against it.
    MaterialNodeTypes RegisterMaterialNodeTypes(NodeCatalog& catalog,
                                                const MaterialShaderInterface& shader);

    // The coercion-aware connection predicate the material catalog supplies to a
    // NodeGraph: exact-TypeId equality plus f32→vecN splat and vec4→vec3/vec2
    // truncate. This is the one place coercion lives; the topology core stays a
    // pure DAG enforcer.
    [[nodiscard]] bool MaterialCanConnect(const PinType& from, const PinType& to);
}
