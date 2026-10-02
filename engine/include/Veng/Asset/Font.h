#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Text/GlyphSource.h>

#include <span>
#include <utility>

namespace Veng::Text
{
    class GlyphAtlas;
}

namespace Veng
{
    /// @brief One glyph's metrics: its advance, its quad relative to the pen, and its atlas page rect.
    ///
    /// Plane bounds place the glyph quad relative to the pen origin on the baseline; the page bounds
    /// give its texel rect (normalized UVs, top-left origin) within the shared dynamic atlas page
    /// named by Page. All spatial fields are in em units except the UV rect, which is in [0, 1] page
    /// space. A whitespace glyph has a zero-size quad and an invalid page; only Advance is
    /// meaningful. A metrics-only lookup (GetGlyphMetrics) leaves the page and UV rect unset; the
    /// ensure-resident lookup (GetGlyph) fills them from the atlas.
    struct FontGlyph
    {
        /// @brief Horizontal advance from this glyph's origin to the next, in em units.
        f32 Advance = 0.0f;
        /// @brief Glyph quad offset from the pen origin (lower-left corner), in em units.
        vec2 PlaneMin{0.0f};
        /// @brief Glyph quad far corner offset from the pen origin (upper-right corner), in em units.
        vec2 PlaneMax{0.0f};
        /// @brief Glyph page rect lower-left corner, in normalized [0, 1] UV space.
        vec2 UvMin{0.0f};
        /// @brief Glyph page rect upper-right corner, in normalized [0, 1] UV space.
        vec2 UvMax{0.0f};
        /// @brief Bindless handle of the shared dynamic atlas page holding the glyph.
        ///
        /// Set by GetGlyph to the page the glyph was packed into. Invalid on a whitespace glyph, on a
        /// metrics-only lookup, and on a glyph the atlas reported not-resident at capacity — the draw
        /// path emits no quad for an invalid page and applies the advance regardless.
        Renderer::TextureHandle Page;
        /// @brief The signed-distance field type this glyph was rasterized into (ensure-resident only).
        Text::GlyphFieldType FieldType = Text::GlyphFieldType::Msdf;
    };

    /// @brief Whether a shape pass measures a run device-free or ensures its glyphs resident to draw.
    ///
    /// Measure reads each glyph's advance from GetGlyphMetrics without touching a graphics device, so
    /// a layout measurement — including a clipped or never-drawn run — rasterizes nothing. Draw
    /// ensures each glyph resident through GetGlyph, so a ShapedGlyph carries the page handle, field
    /// type, and UV rect a quad samples. The advance is identical from both, so a run laid out from
    /// metrics and a run drawn ensure-resident break lines and size identically.
    enum class TextShapeMode : u8
    {
        /// @brief Read advances device-free; emit no page handle or UV rect (layout measurement).
        Measure,
        /// @brief Ensure each glyph resident; emit its page handle, field type, and UV rect (drawing).
        Draw,
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
        /// @brief Where the glyph was placed, in pixels relative to the run origin: x is the pen
        ///        position, y the line's baseline.
        ///
        /// Min and Max are this point offset by the plane bounds of whichever rendition shaped the
        /// run. A draw that ensures the glyph resident later places the atlas rendition's own,
        /// padded bounds about this same point, so a run shaped once device-free still draws exactly
        /// as a run shaped to draw.
        vec2 Pen{0.0f};
        /// @brief Quad top-left corner, in pixels relative to the run origin.
        vec2 Min{0.0f};
        /// @brief Quad bottom-right corner, in pixels relative to the run origin.
        vec2 Max{0.0f};
        /// @brief Atlas page UV of the quad's top-left corner.
        vec2 UvMin{0.0f};
        /// @brief Atlas page UV of the quad's bottom-right corner.
        vec2 UvMax{0.0f};
        /// @brief Bindless handle of the atlas page this glyph samples (Draw mode; invalid otherwise).
        Renderer::TextureHandle Page;
        /// @brief The field type this glyph was rasterized into, selecting the shader coverage branch.
        Text::GlyphFieldType FieldType = Text::GlyphFieldType::Msdf;
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

    /// @brief A face-backed font: an outline the runtime rasterizes on demand, over a shared atlas.
    ///
    /// The font owns a face loaded into the Application's shared GlyphSource and an ordered fallback
    /// chain of sibling fonts whose faces cover what this one does not. GetGlyphMetrics reads a
    /// covered codepoint's advance and box from the resolved face without touching a device; GetGlyph
    /// rasterizes and packs it into the shared dynamic atlas and returns its page. Both walk the
    /// fallback chain — this face first, then each fallback's — and land on .notdef when no face
    /// covers the codepoint. Both are const: the shared GlyphSource/GlyphAtlas are reached through
    /// non-owning pointers, so ensuring residency mutates the shared atlas, not the font.
    ///
    /// The cooked blob carries the face outline bytes, the em-normalized line metrics, a default
    /// field type, an optional hot codepoint set warmed into the shared atlas at load, and the
    /// fallback-font id chain. There is no baked atlas or glyph table: every glyph the text path
    /// draws is rasterized on demand into the shared dynamic atlas. Built by FontLoader from a
    /// CookedFontHeader.
    class Font
    {
    public:
        ~Font();

        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;

        /// @brief Returns the font's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the baseline-to-baseline line height, in em units.
        [[nodiscard]] f32 GetLineHeight() const { return m_LineHeight; }

        /// @brief Returns the ascender height above the baseline, in em units.
        [[nodiscard]] f32 GetAscender() const { return m_Ascender; }

        /// @brief Returns the descender depth below the baseline, in em units (negative below the baseline).
        [[nodiscard]] f32 GetDescender() const { return m_Descender; }

        /// @brief Whether any face in this font's fallback chain covers a codepoint.
        ///
        /// Walks this font's face first, then each fallback's, returning true at the first that has
        /// a glyph for the codepoint. False when no face in the chain covers it (the .notdef case)
        /// or the font carries no runtime face (a font loaded without the shared glyph systems).
        /// @param codepoint  The Unicode codepoint.
        [[nodiscard]] bool HasGlyph(u32 codepoint) const;

        /// @brief Returns a codepoint's advance and box from the covering face, without rasterizing.
        ///
        /// Resolves the codepoint through the fallback chain and reads its metrics from that face
        /// device-free (no rasterization, no atlas touch), so a layout measurement — including a
        /// clipped or never-drawn run — needs no graphics device. Lands on .notdef when no face
        /// covers the codepoint. The returned FontGlyph carries only the advance, plane bounds, and
        /// the resolved face's field type; its page and UV rect are unset.
        /// @param codepoint  The Unicode codepoint.
        [[nodiscard]] FontGlyph GetGlyphMetrics(u32 codepoint) const;

        /// @brief Resolves a codepoint through the fallback chain and ensures it in the shared atlas.
        ///
        /// Rasterizes and packs the glyph into the shared dynamic atlas at the resolved face's field
        /// type — so an SDF-declared fallback rasterizes SDF even when this font is MSDF — and
        /// returns a FontGlyph carrying the atlas page handle, field type, uv rect, plane bounds, and
        /// advance. Lands on .notdef when no face covers the codepoint. The advance matches
        /// GetGlyphMetrics, so a run laid out from metrics and drawn ensure-resident agree; a glyph
        /// the atlas reports not-resident at capacity keeps that advance but leaves its page invalid,
        /// so the draw emits no quad and the box stays stable. Const: the shared atlas is reached
        /// through a non-owning pointer, so residency mutates the atlas.
        /// @param codepoint  The Unicode codepoint.
        /// @param pixelSize  The draw size in pixels (an SDF size bucket; ignored for size-independent MSDF).
        [[nodiscard]] FontGlyph GetGlyph(u32 codepoint, f32 pixelSize) const;

        /// @brief Returns this font's own face in the shared GlyphSource, or FaceId::Invalid.
        [[nodiscard]] Text::FaceId GetFaceId() const { return m_FaceId; }

        /// @brief Returns this font's default glyph field type.
        [[nodiscard]] Text::GlyphFieldType GetFieldType() const { return m_FieldType; }

        /// @brief Returns the shared dynamic atlas this font's glyphs are packed into, or null.
        ///
        /// Null when the font was loaded without the shared glyph systems (a headless manager). The
        /// text draw path reads the atlas's shared sampler and distance range from it for the params
        /// it packs beside each glyph's page handle.
        [[nodiscard]] Text::GlyphAtlas* GetGlyphAtlas() const { return m_GlyphAtlas; }

        /// @brief Returns the kerning adjustment between an ordered codepoint pair, in em units.
        ///
        /// The extra advance added to `left`'s advance when `right` immediately follows it (usually
        /// negative). Read from the covering face's kerning: zero when either codepoint is uncovered,
        /// when the two resolve to different faces (a cross-face pair does not kern), or when the
        /// face carries no kerning for the pair.
        /// @param left   Codepoint of the left glyph.
        /// @param right  Codepoint of the right glyph.
        /// @return The kerning adjustment in em units, or 0 if the pair is not kerned.
        [[nodiscard]] f32 GetKerning(u32 left, u32 right) const;

        /// @brief Shapes a run of text into positioned glyph quads, laid out and line-broken.
        ///
        /// Walks the codepoints applying per-glyph advances and pair kerning, emitting one quad per
        /// visible glyph positioned in pixels relative to the run origin (top-left, y downward).
        /// Explicit newlines ('\n') always break a line; when `maxWidth` is set, the run also
        /// word-wraps to fit within it (breaking at spaces, between two adjacent CJK ideographic
        /// codepoints — the minimal inter-ideograph break, not full line-breaking — and
        /// hard-breaking a word longer than the width). A run with no width constraint stays a single
        /// unwrapped line per newline segment. `mode` selects device-free measurement (advances
        /// only) or an ensure-resident draw (page handles + UV rects); the advance is identical from
        /// both, so a measured extent and a drawn layout agree.
        /// @param codepoints  The Unicode codepoints to shape, in reading order.
        /// @param pixelSize   The em size to render at, in pixels (the em-normalized metrics scale by it).
        /// @param maxWidth    The available width in pixels to wrap within, or nullopt for no wrapping.
        /// @param mode        Whether to measure device-free or ensure each glyph resident to draw.
        /// @return The shaped glyph quads, per-line breakdown, and the run's pixel bounds.
        [[nodiscard]] ShapeResult ShapeRun(std::span<const u32> codepoints, f32 pixelSize,
                                           optional<f32> maxWidth,
                                           TextShapeMode mode = TextShapeMode::Measure) const;

    private:
        friend class FontLoader;

        Font() = default;

        /// @brief A resolved face for a codepoint: the face, its field type, and the glyph index.
        struct ResolvedGlyph
        {
            /// @brief The face covering the codepoint, or this font's face for the .notdef fallback.
            Text::FaceId Face = Text::FaceId::Invalid;
            /// @brief The owning font's field type, so a fallback rasterizes at its own type.
            Text::GlyphFieldType FieldType = Text::GlyphFieldType::Msdf;
            /// @brief The covering face's own glyph index; 0 (.notdef) when no face covers it.
            u32 GlyphIndex = 0;
            /// @brief Whether a face in the chain covers the codepoint.
            bool Covered = false;
        };

        /// @brief Walks the fallback chain for the first face covering a codepoint.
        [[nodiscard]] ResolvedGlyph Resolve(u32 codepoint) const;

        string m_Name;
        f32 m_LineHeight = 0.0f;
        f32 m_Ascender = 0.0f;
        f32 m_Descender = 0.0f;

        /// @brief The face's outline bytes, kept alive because the GlyphSource borrows them.
        vector<u8> m_FaceData;
        /// @brief This font's face in the shared GlyphSource, or Invalid when none was loaded.
        Text::FaceId m_FaceId = Text::FaceId::Invalid;
        /// @brief The default field type glyphs of this font rasterize into.
        Text::GlyphFieldType m_FieldType = Text::GlyphFieldType::Msdf;
        /// @brief The codepoints warmed into the shared atlas at load.
        vector<u32> m_Hotset;
        /// @brief The ordered fallback fonts, kept resident so their faces stay loadable.
        vector<AssetHandle<Font>> m_Fallbacks;
        /// @brief The shared runtime rasterizer; non-owning, null when the font carries no runtime face.
        Text::GlyphSource* m_GlyphSource = nullptr;
        /// @brief The shared dynamic atlas; non-owning, null when the font carries no runtime face.
        Text::GlyphAtlas* m_GlyphAtlas = nullptr;
    };

    /// @brief AssetTypeTrait specialization mapping Font to AssetTypes::Font.
    template <>
    struct AssetTypeTrait<Font>
    {
        /// @brief The asset type tag for Font.
        static constexpr AssetTypeId Type = AssetTypes::Font;
    };
}
