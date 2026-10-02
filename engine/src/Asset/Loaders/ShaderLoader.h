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

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked shader's reflected interface and SPIR-V, naming its vertex
        ///        layout as a dependency; the module is created on the main thread.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
