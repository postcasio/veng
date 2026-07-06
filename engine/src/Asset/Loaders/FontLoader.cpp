#include "FontLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    AssetResult<Detail::LoadJob> FontLoader::Load(AssetManager& /*manager*/,
                                                  Renderer::Context& context, TaskSystem& tasks,
                                                  TypeRegistry& /*types*/, AssetId id,
                                                  std::span<const u8> cooked, bool async) const
    {
        if (cooked.size() < sizeof(CookedFontHeader))
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "font: cooked blob smaller than CookedFontHeader",
            });
        }

        CookedFontHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedFontVersion)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = fmt::format("font: blob version {} != expected {}", header.Version,
                                      CookedFontVersion),
            });
        }

        // The cooker emits an uncompressed RGBA8Unorm atlas (Renderer::Format ordinal 2): the MSDF
        // needs the raw three channels, so no block-compressed format is valid here.
        if (header.AtlasFormat != static_cast<u32>(Renderer::Format::RGBA8Unorm))
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = fmt::format("font: unexpected atlas format ordinal {} (expected "
                                      "RGBA8Unorm)",
                                      header.AtlasFormat),
            });
        }

        const usize glyphBytes = static_cast<usize>(header.GlyphCount) * sizeof(CookedGlyph);
        const usize kernBytes = static_cast<usize>(header.KerningCount) * sizeof(CookedKernPair);
        const usize atlasBytes =
            static_cast<usize>(header.AtlasWidth) * header.AtlasHeight * 4; // RGBA8

        if (cooked.size() < sizeof(header) + glyphBytes + kernBytes + atlasBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "font: cooked blob smaller than header + tables + atlas texels",
            });
        }

        const u8* cursor = cooked.data() + sizeof(header);

        Ref<Font> font(new Font());
        font->m_Name = fmt::format("Font {}", id.Value);
        font->m_AtlasExtent = {header.AtlasWidth, header.AtlasHeight};
        font->m_DistanceRange = header.DistanceRange;
        font->m_LineHeight = header.LineHeight;
        font->m_Ascender = header.Ascender;
        font->m_Descender = header.Descender;

        // The atlas UVs normalize each glyph's texel rect against the atlas dimensions once here, so
        // ShapeRun and drawing never repeat the divide. A top-left texel origin matches the cooked
        // atlas's row-major top-to-bottom layout.
        const f32 invWidth =
            header.AtlasWidth > 0 ? 1.0f / static_cast<f32>(header.AtlasWidth) : 0.0f;
        const f32 invHeight =
            header.AtlasHeight > 0 ? 1.0f / static_cast<f32>(header.AtlasHeight) : 0.0f;

        for (u32 i = 0; i < header.GlyphCount; i++)
        {
            CookedGlyph cooked_glyph;
            std::memcpy(&cooked_glyph, cursor, sizeof(cooked_glyph));
            cursor += sizeof(cooked_glyph);

            FontGlyph glyph;
            glyph.Advance = cooked_glyph.Advance;
            glyph.PlaneMin = {cooked_glyph.PlaneLeft, cooked_glyph.PlaneBottom};
            glyph.PlaneMax = {cooked_glyph.PlaneLeft + cooked_glyph.PlaneWidth,
                              cooked_glyph.PlaneBottom + cooked_glyph.PlaneHeight};
            glyph.UvMin = {cooked_glyph.AtlasLeft * invWidth, cooked_glyph.AtlasTop * invHeight};
            glyph.UvMax = {(cooked_glyph.AtlasLeft + cooked_glyph.AtlasWidth) * invWidth,
                           (cooked_glyph.AtlasTop + cooked_glyph.AtlasHeight) * invHeight};
            font->m_Glyphs.emplace(cooked_glyph.Codepoint, glyph);
        }

        for (u32 i = 0; i < header.KerningCount; i++)
        {
            CookedKernPair pair;
            std::memcpy(&pair, cursor, sizeof(pair));
            cursor += sizeof(pair);
            font->m_Kerning.emplace(std::pair<u32, u32>{pair.Left, pair.Right}, pair.Advance);
        }

        // The atlas is an ordinary bindless RGBA8 texture, so it rides the exact texture-upload path
        // — worker-legal create + upload here, main-thread bindless registration in Finalize. A
        // clamp-to-edge linear sampler suits the padded MSDF atlas.
        const TextureData atlasData{
            .Name = font->m_Name + " Atlas",
            .Extent = {header.AtlasWidth, header.AtlasHeight},
            .Format = Renderer::Format::RGBA8Unorm,
            .MipLevels = 1,
            .Pixels = cooked.subspan(sizeof(header) + glyphBytes + kernBytes, atlasBytes),
            .Sampler =
                {
                    .MagFilter = Renderer::Filter::Linear,
                    .MinFilter = Renderer::Filter::Linear,
                    .MipmapMode = Renderer::MipmapMode::Linear,
                    .AddressModeU = Renderer::AddressMode::ClampToEdge,
                    .AddressModeV = Renderer::AddressMode::ClampToEdge,
                    .AddressModeW = Renderer::AddressMode::ClampToEdge,
                },
            .ChannelLayout = CookedChannelLayout::Direct,
        };

        if (async)
        {
            Task<void> upload;
            font->m_Atlas = Texture::PrepareAsync(context, atlasData, tasks, upload);
        }
        else
        {
            font->m_Atlas = Texture::PrepareSync(context, atlasData);
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(font),
            .Dependencies = {},
            .Finalize = [font]() -> VoidResult
            {
                font->m_Atlas->Finalize();
                return {};
            },
        };
    }
}
