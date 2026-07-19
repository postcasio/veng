#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>

#include <span>
#include <utility>

namespace Veng
{
    class Texture;

    /// @brief One glyph's metrics: its advance, its quad relative to the pen, and its atlas rect.
    ///
    /// Plane bounds place the glyph quad relative to the pen origin on the baseline; atlas bounds
    /// give its texel rect in the MSDF atlas (normalized UVs, top-left origin). All spatial fields
    /// are in em units except the UV rect, which is in [0, 1] atlas space. A whitespace glyph has
    /// a zero-size quad and a zero atlas rect; only Advance is meaningful.
    struct FontGlyph
    {
        /// @brief Horizontal advance from this glyph's origin to the next, in em units.
        f32 Advance = 0.0f;
        /// @brief Glyph quad offset from the pen origin (lower-left corner), in em units.
        vec2 PlaneMin{0.0f};
        /// @brief Glyph quad far corner offset from the pen origin (upper-right corner), in em units.
        vec2 PlaneMax{0.0f};
        /// @brief Glyph atlas rect lower-left corner, in normalized [0, 1] UV space.
        vec2 UvMin{0.0f};
        /// @brief Glyph atlas rect upper-right corner, in normalized [0, 1] UV space.
        vec2 UvMax{0.0f};
    };

    /// @brief One positioned glyph quad produced by shaping a run of text.
    ///
    /// Position is in pixels, relative to the run origin (the top-left of the shaped block): Min is
    /// the quad's top-left, Max its bottom-right, with y increasing downward. Uv* address the glyph
    /// in the MSDF atlas. A whitespace glyph produces no quad (shaping advances the pen but emits
    /// nothing), so every entry here has geometry.
    struct ShapedGlyph
    {
        /// @brief Codepoint this quad renders.
        u32 Codepoint = 0;
        /// @brief Quad top-left corner, in pixels relative to the run origin.
        vec2 Min{0.0f};
        /// @brief Quad bottom-right corner, in pixels relative to the run origin.
        vec2 Max{0.0f};
        /// @brief Atlas UV of the quad's top-left corner.
        vec2 UvMin{0.0f};
        /// @brief Atlas UV of the quad's bottom-right corner.
        vec2 UvMax{0.0f};
    };

    /// @brief Per-line geometry produced by shaping: the line's glyph span and its extent.
    ///
    /// Glyphs[Start, Start + Count) in the ShapeResult belong to this line. Width is the line's
    /// advance width in pixels; Baseline is the line's baseline y in pixels, relative to the run
    /// origin (y increases downward, so the first line's baseline is roughly the ascender height).
    struct ShapedLine
    {
        /// @brief Index of this line's first glyph in ShapeResult::Glyphs.
        u32 Start = 0;
        /// @brief Number of glyphs on this line.
        u32 Count = 0;
        /// @brief The line's advance width in pixels.
        f32 Width = 0.0f;
        /// @brief The line's baseline y in pixels, relative to the run origin.
        f32 Baseline = 0.0f;
    };

    /// @brief The result of shaping a run of text: its positioned glyph quads, line breakdown, and bounds.
    struct ShapeResult
    {
        /// @brief The positioned, visible glyph quads across every line, in reading order.
        vector<ShapedGlyph> Glyphs;
        /// @brief The lines the run broke into, in top-to-bottom order.
        vector<ShapedLine> Lines;
        /// @brief The run's bounding size in pixels: the widest line's width and the total block height.
        vec2 Size{0.0f};
    };

    /// @brief A resident MSDF font: a bindless glyph atlas plus CPU metrics for shaping and drawing.
    ///
    /// The atlas is an ordinary RGBA8 texture registered into the bindless set (set 0); its three
    /// colour channels carry the multi-channel signed-distance field a text shader samples. The CPU
    /// metrics — the per-glyph table, kerning lookup, and line metrics, all em-normalized — drive
    /// ShapeRun, the single device-free shaping/wrapping path text drawing and layout measurement
    /// share. Built by FontLoader from a CookedFontHeader; GetAtlasHandle() is valid once the
    /// atlas has been finalized into the bindless registry.
    class Font
    {
    public:
        ~Font();

        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;

        /// @brief Returns the font's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the MSDF atlas dimensions in pixels.
        [[nodiscard]] uvec2 GetAtlasExtent() const { return m_AtlasExtent; }

        /// @brief Returns the distance range, in atlas pixels, baked into the SDF.
        ///
        /// A text shader converts a sampled signed distance to a screen-space coverage using this
        /// range: it is the pixel width the distance field transitions across at the atlas's native
        /// scale, and the shader divides screen-space distance by it (scaled by the draw size).
        [[nodiscard]] f32 GetDistanceRange() const { return m_DistanceRange; }

        /// @brief Returns the baseline-to-baseline line height, in em units.
        [[nodiscard]] f32 GetLineHeight() const { return m_LineHeight; }

        /// @brief Returns the ascender height above the baseline, in em units.
        [[nodiscard]] f32 GetAscender() const { return m_Ascender; }

        /// @brief Returns the descender depth below the baseline, in em units (negative below the baseline).
        [[nodiscard]] f32 GetDescender() const { return m_Descender; }

        /// @brief Returns the number of glyphs in the font's charset.
        [[nodiscard]] usize GetGlyphCount() const { return m_Glyphs.size(); }

        /// @brief Looks up a glyph by codepoint.
        /// @param codepoint  The Unicode codepoint.
        /// @return The glyph's metrics, or nullptr if the codepoint is not in the cooked charset.
        [[nodiscard]] const FontGlyph* GetGlyph(u32 codepoint) const;

        /// @brief Returns the kerning adjustment between an ordered codepoint pair, in em units.
        ///
        /// The extra advance added to `left`'s advance when `right` immediately follows it (usually
        /// negative). Zero when the pair is not kerned.
        /// @param left   Codepoint of the left glyph.
        /// @param right  Codepoint of the right glyph.
        /// @return The kerning adjustment in em units, or 0 if the pair is not kerned.
        [[nodiscard]] f32 GetKerning(u32 left, u32 right) const;

        /// @brief Returns the bindless texture handle of the MSDF atlas (valid after the atlas finalizes).
        [[nodiscard]] Renderer::TextureHandle GetAtlasHandle() const;

        /// @brief Returns the bindless sampler handle of the MSDF atlas (valid after the atlas finalizes).
        [[nodiscard]] Renderer::SamplerHandle GetAtlasSamplerHandle() const;

        /// @brief Shapes a run of text into positioned glyph quads, laid out and line-broken.
        ///
        /// Walks the codepoints applying per-glyph advances and pair kerning, emitting one quad per
        /// visible glyph positioned in pixels relative to the run origin (top-left, y downward).
        /// Explicit newlines ('\n') always break a line; when `maxWidth` is set, the run also
        /// word-wraps to fit within it (breaking at spaces, and hard-breaking a word longer than
        /// the width). A run with no width constraint stays a single unwrapped line per newline
        /// segment. Device-free — the one shaping path both text drawing and layout measurement
        /// call, so a measured extent and a drawn layout agree.
        /// @param codepoints  The Unicode codepoints to shape, in reading order.
        /// @param pixelSize   The em size to render at, in pixels (the em-normalized metrics scale by it).
        /// @param maxWidth    The available width in pixels to wrap within, or nullopt for no wrapping.
        /// @return The shaped glyph quads, per-line breakdown, and the run's pixel bounds.
        [[nodiscard]] ShapeResult ShapeRun(std::span<const u32> codepoints, f32 pixelSize,
                                           optional<f32> maxWidth) const;

    private:
        friend class FontLoader;

        Font() = default;

        string m_Name;
        uvec2 m_AtlasExtent{0};
        f32 m_DistanceRange = 0.0f;
        f32 m_LineHeight = 0.0f;
        f32 m_Ascender = 0.0f;
        f32 m_Descender = 0.0f;

        /// @brief Codepoint → glyph metrics.
        map<u32, FontGlyph> m_Glyphs;
        /// @brief Ordered (left, right) codepoint pair → kerning adjustment in em units.
        map<std::pair<u32, u32>, f32> m_Kerning;

        /// @brief The MSDF atlas, a bindless RGBA8 texture the atlas handles delegate to.
        Ref<Texture> m_Atlas;
    };

    /// @brief AssetTypeTrait specialization mapping Font to AssetTypes::Font.
    template <>
    struct AssetTypeTrait<Font>
    {
        /// @brief The asset type tag for Font.
        static constexpr AssetTypeId Type = AssetTypes::Font;
    };
}
