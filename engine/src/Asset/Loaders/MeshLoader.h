#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Renderer/Mesh.h>

// The AssetType::Mesh loader (planset-5 plan 07): a CookedMeshHeader + attribute
// descriptor + submesh table + interleaved vertex/index buffers -> a
// Renderer::Mesh with two GPU buffers (UploadSync), after validating the cooked
// layout against the engine's canonical VertexBufferLayout.

namespace Veng
{
    class MeshLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Mesh; }

        [[nodiscard]] AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const override;
    };
}
