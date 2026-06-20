#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Mesh.h>

namespace Veng
{
    /// @brief AssetType::Mesh loader.
    ///
    /// Decodes a CookedMeshHeader + attribute descriptor + submesh table + interleaved
    /// vertex/index buffers into a Veng::Mesh with two GPU buffers, after validating
    /// the cooked layout against the engine's canonical VertexBufferLayout.
    class MeshLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::Mesh.
        [[nodiscard]] AssetType Type() const override { return AssetType::Mesh; }

        /// @brief Decodes the cooked mesh blob into a LoadJob producing a resident Veng::Mesh.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
