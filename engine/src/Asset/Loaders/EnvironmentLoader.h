#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Environment.h>

namespace Veng
{
    /// @brief AssetType::Environment loader.
    ///
    /// Decodes a CookedEnvironmentHeader + HDR panorama pixels into a Veng::EnvironmentMap.
    /// Image creation and upload are worker-legal; bindless registration is deferred to the
    /// main-thread Finalize so the handle is assigned on the correct thread.
    class EnvironmentLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::Environment.
        [[nodiscard]] AssetType Type() const override { return AssetType::Environment; }

        /// @brief Decodes the cooked environment blob into a LoadJob producing a resident Veng::EnvironmentMap.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
