#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/ShaderInterface.h>

// ShaderAsset: a loaded shader module + its reflected
// ShaderInterface, as cooked by the Slang importer (or supplied inline for
// editor-produced shaders). Pipelines build their descriptor-set/pipeline
// layouts from Interface rather than a hand-declared one.
namespace Veng
{
    struct ShaderAsset
    {
        Ref<Renderer::Shader> Module;
        Renderer::ShaderInterface Interface;
    };

    template <>
    struct AssetTypeTrait<ShaderAsset>
    {
        static constexpr AssetType Type = AssetType::Shader;
    };
}
