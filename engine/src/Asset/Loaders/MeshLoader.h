#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Mesh.h>

namespace Veng
{
    /// @brief AssetTypes::Mesh loader.
    ///
    /// Decodes a CookedMeshHeader + attribute descriptor + submesh table + socket table +
    /// interleaved vertex/index buffers into a Veng::Mesh with two GPU buffers, after
    /// validating the blob's format version and the cooked layout against the engine's
    /// canonical VertexBufferLayout.
    class MeshLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Mesh.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Mesh; }

        /// @brief Decodes the cooked mesh blob into a LoadJob producing a resident Veng::Mesh.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
