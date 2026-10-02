#include "FontLoader.h"

#include <cstring>
#include <span>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
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

    AssetResult<Detail::ParsedAsset> FontLoader::Parse(const AssetParseContext& /*context*/,
                                                       const AssetId id,
                                                       const std::span<const u8> cooked) const
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

        const auto faceBytes = static_cast<usize>(header.FaceBytes);
        const usize hotsetBytes = static_cast<usize>(header.HotsetCount) * sizeof(u32);
        const usize fallbackBytes = static_cast<usize>(header.FallbackCount) * sizeof(u64);

        if (cooked.size() < sizeof(header) + faceBytes + hotsetBytes + fallbackBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "font: cooked blob smaller than header + face + hot set + fallbacks",
            });
        }

        const Ref<Font> font(new Font());
        font->m_Name = fmt::format("Font {}", id.Value);
        font->m_LineHeight = header.LineHeight;
        font->m_Ascender = header.Ascender;
        font->m_Descender = header.Descender;
        font->m_FieldType = static_cast<Text::GlyphFieldType>(header.FieldType);

        // The face bytes drive the runtime rasterizer, so they are kept on the font (the shared
        // GlyphSource borrows them). The hot set and fallback ids follow them.
        const usize faceOffset = sizeof(header);
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

        // The fallback chain: each is an ordinary Font dependency kept resident, so its face is
        // loaded and its glyphs reachable when this font resolves a codepoint through it.
        const usize fallbackOffset = hotsetOffset + hotsetBytes;
        Detail::ParsedAsset parsed;
        for (u32 i = 0; i < header.FallbackCount; i++)
        {
            u64 fallbackId = 0;
            std::memcpy(&fallbackId, cooked.data() + fallbackOffset + i * sizeof(u64), sizeof(u64));
            parsed.Dependencies.push_back({.Type = AssetTypes::Font, .Id = AssetId{fallbackId}});
        }

        parsed.Complete = [font](AssetManager& manager,
                                 std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            // The shared rasterizer and atlas are wired only in a running application; a headless
            // manager (a cook-side test) leaves them null and the font has no drawable glyphs.
            font->m_GlyphSource = manager.GetGlyphSource();
            font->m_GlyphAtlas = manager.GetGlyphAtlas();
            for (const Ref<Detail::AssetCacheEntry>& fallback : resolved)
            {
                font->m_Fallbacks.push_back(AssetManager::HandleOf<Font>(fallback));
            }
            return Detail::LoadJob{
                .Resource = Detail::RefAny(font),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
                .Finalize = [font]() -> VoidResult
                {
                    // Load the face into the shared rasterizer and warm the hot set into the shared
                    // atlas, both main-thread steps. Absent glyph systems (a headless manager) leave the
                    // font faceless, so it draws nothing.
                    if (font->m_GlyphSource != nullptr && !font->m_FaceData.empty())
                    {
                        const Result<Text::FaceId> face = font->m_GlyphSource->LoadFace(
                            std::as_bytes(std::span(font->m_FaceData)));
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
                                static_cast<void>(font->GetGlyph(codepoint, HotSetWarmSize));
                            }
                        }
                    }
                    return {};
                },
            };
        };
        return parsed;
    }
}
