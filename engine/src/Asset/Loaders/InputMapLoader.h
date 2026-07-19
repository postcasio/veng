#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedInputMapHeader blob into a CPU-only InputMappingContext asset.
    ///
    /// No GPU resource and no dependencies: the actions + bindings record is decoded through the
    /// shared reflection reader into a Ref<InputMappingContext>. A seat's InputContextStack
    /// resolves its contexts through the ordinary load path.
    class InputMapLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::InputMap.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::InputMap; }

        /// @brief Decodes a cooked input-map blob into a Ref<InputMappingContext>.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
