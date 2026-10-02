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

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked mesh into its vertex and index buffers, naming its materials
        ///        and skeleton as dependencies.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
