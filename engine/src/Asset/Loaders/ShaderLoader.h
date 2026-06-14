#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Shader.h>

// The AssetType::Shader loader: a CookedShaderHeader +
// reflected interface + SPIR-V -> a Veng::Shader (Shader module +
// ShaderInterface), reconstructing the interface's bindings/push
// constants/vertex inputs from the cooked, underlying-integer enum fields.

namespace Veng
{
    class ShaderLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Shader; }

        [[nodiscard]] AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const override;
    };
}
