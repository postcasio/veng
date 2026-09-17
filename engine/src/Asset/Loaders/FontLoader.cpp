#include "FontLoader.h"

#include <cstring>
#include <span>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

namespace Veng
{
    namespace
    {
        // A representative rasterization size the hot set warms at. MSDF is size-independent, so its
        // key ignores this; an SDF hot set quantizes it to a bucket.
        constexpr f32 HotSetWarmSize = 48.0f;
    }

    AssetResult<Detail::LoadJob> FontLoader::Load(AssetManager& manager, Renderer::Context& context,
                                                  TaskSystem& tasks, TypeRegistry& /*types*/,
                                                  AssetId id, std::span<const u8> cooked,
                                                  bool async) const
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
        const auto faceBytes = static_cast<usize>(header.FaceBytes);
        const usize hotsetBytes = static_cast<usize>(header.HotsetCount) * sizeof(u32);
        const usize fallbackBytes = static_cast<usize>(header.FallbackCount) * sizeof(u64);

        if (cooked.size() < sizeof(header) + glyphBytes + kernBytes + atlasBytes + faceBytes +
                                hotsetBytes + fallbackBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "font: cooked blob smaller than header + tables + atlas texels + face",
            });
        }

        const u8* cursor = cooked.data() + sizeof(header);

        const Ref<Font> font(new Font());
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
            // The reserved .notdef entry becomes the tofu box; it is not a real codepoint, so it
            // stays out of the glyph map (GetGlyph on the sentinel is an absent lookup like any
            // other), and ShapeRun reaches it through GetNotdefGlyph().
            if (cooked_glyph.Codepoint == CookedFontNotdefCodepoint)
            {
                font->m_Notdef = glyph;
                continue;
            }
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

        // The face bytes, hot set, and fallback ids follow the atlas texels. The face bytes are kept
        // on the font because the shared GlyphSource borrows them; the runtime rasterizes any covered
        // codepoint from them, resolving a missing one through the fallback chain.
        const usize faceOffset = sizeof(header) + glyphBytes + kernBytes + atlasBytes;
        font->m_FieldType = static_cast<Text::GlyphFieldType>(header.FieldType);
        font->m_FaceData.resize(faceBytes);
        if (faceBytes > 0)
        {
            std::memcpy(font->m_FaceData.data(), cooked.data() + faceOffset, faceBytes);
        }

        const usize hotsetOffset = faceOffset + faceBytes;
        font->m_Hotset.resize(header.HotsetCount);
        if (header.HotsetCount > 0)
        {
            std::memcpy(font->m_Hotset.data(), cooked.data() + hotsetOffset, hotsetBytes);
        }

        // The shared rasterizer and atlas are wired only in a running application; a headless
        // manager (a cook-side test) leaves them null and the font keeps just its cooked atlas.
        font->m_GlyphSource = manager.GetGlyphSource();
        font->m_GlyphAtlas = manager.GetGlyphAtlas();

        // Resolve the fallback chain: each is an ordinary Font dependency kept resident, so its face
        // is loaded and its glyphs reachable when this font resolves a codepoint through it.
        const usize fallbackOffset = hotsetOffset + hotsetBytes;
        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        for (u32 i = 0; i < header.FallbackCount; i++)
        {
            u64 fallbackId = 0;
            std::memcpy(&fallbackId, cooked.data() + fallbackOffset + i * sizeof(u64), sizeof(u64));

            if (async)
            {
                AssetHandle<Font> handle = manager.Load<Font>(AssetId{fallbackId});
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(
                        AssetLoadError{.Kind = AssetError::MissingDependency,
                                       .Id = AssetId{fallbackId},
                                       .Detail = fmt::format("font {}: fallback {} did not resolve",
                                                             id.Value, fallbackId)});
                }
                dependencies.push_back(AssetManager::EntryOf(handle));
                font->m_Fallbacks.push_back(std::move(handle));
            }
            else
            {
                AssetResult<AssetHandle<Font>> handle = manager.LoadSync<Font>(AssetId{fallbackId});
                if (!handle)
                {
                    return std::unexpected(handle.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*handle));
                font->m_Fallbacks.push_back(std::move(*handle));
            }
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(font),
            .Dependencies = std::move(dependencies),
            .Finalize = [font]() -> VoidResult
            {
                font->m_Atlas->Finalize();

                // Load the face into the shared rasterizer and warm the hot set into the shared
                // atlas, both main-thread steps. Absent glyph systems (a headless manager) leave the
                // font with just its cooked atlas.
                if (font->m_GlyphSource != nullptr && !font->m_FaceData.empty())
                {
                    const Result<Text::FaceId> face =
                        font->m_GlyphSource->LoadFace(std::as_bytes(std::span(font->m_FaceData)));
                    if (!face)
                    {
                        return std::unexpected(face.error());
                    }
                    font->m_FaceId = *face;

                    if (font->m_GlyphAtlas != nullptr)
                    {
                        for (const u32 codepoint : font->m_Hotset)
                        {
                            // The residency side effect is the point; the returned glyph is unused.
                            static_cast<void>(font->EnsureGlyph(codepoint, HotSetWarmSize));
                        }
                    }
                }
                return {};
            },
        };
    }
}
