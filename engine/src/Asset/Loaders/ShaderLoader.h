#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Shader.h>

namespace Veng
{
    /// @brief AssetTypes::Shader loader.
    ///
    /// Decodes a CookedShaderHeader + reflected interface + SPIR-V into a Veng::Shader
    /// (ShaderModule + ShaderInterface), bridging the cooked underlying-integer enum
    /// fields to Veng::Renderer enums for bindings, push constants, and vertex inputs.
    class ShaderLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Shader.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Shader; }

        /// @brief Decodes the cooked shader blob into a LoadJob producing a resident Veng::Shader.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
