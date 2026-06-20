#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/ShaderInterface.h>

namespace Veng
{
    /// @brief A loaded shader module paired with its reflected interface.
    ///
    /// Produced by the Slang importer (or supplied inline for editor-built shaders).
    /// Pipelines derive their descriptor-set and pipeline layouts from Interface
    /// rather than a hand-declared one.
    struct Shader
    {
        /// @brief The compiled SPIR-V module.
        Ref<Renderer::ShaderModule> Module;
        /// @brief Reflected parameter/binding layout.
        Renderer::ShaderInterface Interface;
    };

    /// @brief AssetTypeTrait specialization mapping Shader to AssetType::Shader.
    template <>
    struct AssetTypeTrait<Shader>
    {
        /// @brief The asset type tag for Shader.
        static constexpr AssetType Type = AssetType::Shader;
    };
}
