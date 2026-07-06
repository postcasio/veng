#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Font.h>

namespace Veng
{
    /// @brief AssetType::Font loader.
    ///
    /// Decodes a CookedFontHeader + glyph/kerning tables + MSDF atlas texels into a Veng::Font.
    /// The atlas image is created and uploaded through the ordinary texture path (worker-legal);
    /// its bindless registration is deferred to the main-thread Finalize so the handle is assigned
    /// on the correct thread.
    class FontLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::Font.
        [[nodiscard]] AssetType Type() const override { return AssetType::Font; }

        /// @brief Decodes the cooked font blob into a LoadJob producing a resident Veng::Font.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
