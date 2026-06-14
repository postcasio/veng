#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Mesh.h>

// The AssetType::Mesh loader: a CookedMeshHeader + attribute
// descriptor + submesh table + interleaved vertex/index buffers -> a
// Veng::Mesh with two GPU buffers (UploadSync), after validating the cooked
// layout against the engine's canonical VertexBufferLayout.

namespace Veng
{
    class MeshLoader final : public AssetLoader
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Mesh; }

        [[nodiscard]] AssetResult<Detail::LoadJob> Load(
            AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
            AssetId id, std::span<const u8> cooked, bool async) const override;
    };
}
