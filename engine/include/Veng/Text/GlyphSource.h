#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>

#include <cstddef>
#include <span>

namespace Veng::Text
{
    /// @brief The signed-distance representation a glyph is rasterized into.
    ///
    /// Selected per glyph (D2): the atlas keys pages by it and the text vertex carries it, so a
    /// single run may mix both fields. The choice is a quality/size trade, not a coverage one —
    /// both encode the same glyph outline over the same distance range, so the shader coverage math
    /// divides by one range constant regardless of which field a glyph carries.
    enum class GlyphFieldType : u8
    {
        /// @brief A three-channel multi-channel signed distance field (msdfgen). Crisp at any scale
        /// and corner-preserving, at ~3-4x the bytes of a single-channel field. Sampled as the
        /// median of its three channels.
        Msdf,
        /// @brief A single-channel true signed distance field (FreeType's SDF renderer). Cheap to
        /// generate and small, softening hard corners at extreme magnification.
        Sdf,
    };

    /// @brief An opaque handle to a face loaded into a GlyphSource's own face table.
    ///
    /// Valid only for the GlyphSource that minted it, and only while that source is alive. The
    /// underlying font bytes are borrowed, not owned (see GlyphSource::LoadFace), so the handle is
    /// no more valid than the bytes behind it.
    enum class FaceId : u32
    {
        /// @brief The never-valid handle.
        Invalid = 0xFFFFFFFFu,
    };

    /// @brief A face's global metrics, em-normalized and read once at load.
    ///
    /// Every vertical field is in em units (the em is 1.0), so a consumer scales them to pixels by
    /// a single multiply. The raw units-per-em is kept for callers that must map font-unit values.
    struct FaceMetrics
    {
        /// @brief The face's design units per em, straight from the font header.
        f32 UnitsPerEm = 0.0f;
        /// @brief The ascender height above the baseline, in em units.
        f32 Ascender = 0.0f;
        /// @brief The descender depth below the baseline, in em units (negative below the baseline).
        f32 Descender = 0.0f;
        /// @brief The baseline-to-baseline line height, in em units.
        f32 LineHeight = 0.0f;
    };

    /// @brief One glyph rasterized to CPU pixels plus its size-independent placement metrics.
    ///
    /// The pixels are row-major and top-down (row 0 is the top), tightly packed at Channels bytes
    /// per texel — one channel for Sdf, four for Msdf (msdfgen's three MSDF channels expanded to
    /// RGBA8 with an opaque alpha, since a three-channel texture is not reliably sampleable across
    /// backends). Every spatial value is in em units and independent of the rasterization pixel
    /// size, so the caller places the quad and only then chooses a draw size: the pixel size is a
    /// resolution, not a layout size. A whitespace or empty glyph has zero-size pixels and
    /// degenerate plane bounds; only its advance is meaningful.
    struct RasterizedGlyph
    {
        /// @brief The field representation these pixels encode.
        GlyphFieldType FieldType = GlyphFieldType::Msdf;
        /// @brief Bitmap width in texels.
        u32 Width = 0;
        /// @brief Bitmap height in texels.
        u32 Height = 0;
        /// @brief Bytes per texel: 1 for Sdf, 4 for Msdf.
        u32 Channels = 0;
        /// @brief Glyph quad lower-left corner offset from the pen origin, in em units.
        vec2 PlaneMin{0.0f};
        /// @brief Glyph quad upper-right corner offset from the pen origin, in em units.
        vec2 PlaneMax{0.0f};
        /// @brief Horizontal advance from this glyph's origin to the next, in em units.
        f32 Advance = 0.0f;
        /// @brief The bitmap bytes, Width * Height * Channels of them, row-major top-down.
        vector<u8> Pixels;
    };

    /// @brief A glyph's size-independent placement metrics, read without rasterizing.
    ///
    /// The advance and the tight outline bounding box, both em-normalized (the em is 1.0) and y-up
    /// with the baseline at 0 — everything a layout pass needs to place and advance a glyph without
    /// touching a graphics device. The advance is identical to the one a Rasterize of the same glyph
    /// reports; the plane bounds are the outline's own box, which the rasterized field grows by half
    /// the distance range, so they are a measurement box, not the drawn quad. A whitespace glyph has
    /// a zero advance-only box.
    struct GlyphMetrics
    {
        /// @brief Horizontal advance from this glyph's origin to the next, in em units.
        f32 Advance = 0.0f;
        /// @brief Outline box lower-left corner offset from the pen origin, in em units.
        vec2 PlaneMin{0.0f};
        /// @brief Outline box upper-right corner offset from the pen origin, in em units.
        vec2 PlaneMax{0.0f};
    };

    /// @brief The runtime glyph rasterizer: faces in, one glyph's pixels and metrics out.
    ///
    /// Owns the FreeType library and, per loaded face, an msdfgen wrapper over the same FreeType
    /// face — both hidden behind the Native idiom so no backend type reaches this header. Given a
    /// loaded face, a glyph index, a pixel size and a field type, Rasterize returns a single glyph's
    /// CPU bitmap and em-space metrics; it packs nothing, uploads nothing, and touches no graphics
    /// device, so it runs headless. This is the deliberate, documented exception that brings a font
    /// rasterizer into libveng — the price of rendering any codepoint a face covers on demand,
    /// rather than only a charset baked offline.
    ///
    /// Not thread-safe: FreeType's library is not safe across concurrent face access, so every
    /// entry point must be called from a single thread at a time (the caller serializes any worker
    /// offload). The source borrows each face's bytes and never copies them, so those bytes must
    /// outlive the face.
    class GlyphSource
    {
    public:
        /// @brief Initializes the FreeType library. Fails if FreeType cannot be brought up.
        GlyphSource();
        ~GlyphSource();

        GlyphSource(const GlyphSource&) = delete;
        GlyphSource& operator=(const GlyphSource&) = delete;
        GlyphSource(GlyphSource&&) = delete;
        GlyphSource& operator=(GlyphSource&&) = delete;

        /// @brief Loads a face from in-memory outline bytes into the source's face table.
        ///
        /// Reads the face directly from memory — no file path, no filesystem access — so the bytes
        /// must remain valid for as long as the returned face is used; the source borrows them. The
        /// face's global metrics are read once here.
        /// @param fontData  The font file's bytes (TrueType/OpenType outline data), kept alive by the caller.
        /// @return A handle to the loaded face, or an error string when the bytes are not a loadable face.
        [[nodiscard]] Result<FaceId> LoadFace(std::span<const std::byte> fontData);

        /// @brief Returns a loaded face's em-normalized global metrics.
        /// @param face  A face handle from LoadFace.
        [[nodiscard]] FaceMetrics GetFaceMetrics(FaceId face) const;

        /// @brief Whether a face covers a codepoint (has a non-.notdef glyph for it).
        ///
        /// The predicate a fallback chain walks to pick the first face covering a codepoint.
        /// @param face       A face handle from LoadFace.
        /// @param codepoint  The Unicode codepoint.
        [[nodiscard]] bool HasGlyph(FaceId face, u32 codepoint) const;

        /// @brief Returns a face's own glyph index for a codepoint, or 0 (.notdef) when uncovered.
        ///
        /// A resolved glyph is addressed by index thereafter — Rasterize and GetKerning take an
        /// index, not a codepoint, so a covered glyph is looked up once.
        /// @param face       A face handle from LoadFace.
        /// @param codepoint  The Unicode codepoint.
        [[nodiscard]] u32 GlyphIndex(FaceId face, u32 codepoint) const;

        /// @brief Returns a glyph's advance and outline box in em units, without rasterizing it.
        ///
        /// Loads the outline for its metrics only — no field is generated, no bitmap allocated, no
        /// graphics device touched — so a layout measurement stays device-free. The advance matches
        /// the one Rasterize reports for the same glyph.
        /// @param face        A face handle from LoadFace.
        /// @param glyphIndex  The face's glyph index (from GlyphIndex).
        [[nodiscard]] GlyphMetrics GetGlyphMetrics(FaceId face, u32 glyphIndex) const;

        /// @brief Rasterizes one glyph into a CPU bitmap plus its em-space metrics.
        ///
        /// The Msdf path loads the outline, edge-colors it, and generates a three-channel field at
        /// the given pixel size; the Sdf path renders through FreeType's SDF mode with its spread
        /// set to the same distance range. Both encode the same distance range in pixels, so one
        /// shader range constant serves either. The pixel size is only the rasterization resolution;
        /// the returned metrics are em-normalized and size-independent.
        /// @param face             A face handle from LoadFace.
        /// @param glyphIndex       The face's glyph index (from GlyphIndex).
        /// @param pixelSize        The em size to rasterize at, in pixels.
        /// @param fieldType        Which field representation to produce.
        /// @param distanceRangePx  The signed-distance range, in pixels, both fields encode.
        /// @return The rasterized glyph, or an error string when the glyph cannot be rendered.
        [[nodiscard]] Result<RasterizedGlyph> Rasterize(FaceId face, u32 glyphIndex, f32 pixelSize,
                                                        GlyphFieldType fieldType,
                                                        u32 distanceRangePx);

        /// @brief Returns the kerning adjustment between an ordered glyph-index pair, in em units.
        ///
        /// The extra advance added after the left glyph when the right glyph immediately follows it
        /// (usually negative). Zero when the pair is not kerned or the face carries no kerning.
        /// @param face        A face handle from LoadFace.
        /// @param leftIndex   The left glyph's index.
        /// @param rightIndex  The right glyph's index.
        [[nodiscard]] f32 GetKerning(FaceId face, u32 leftIndex, u32 rightIndex) const;

    private:
        struct Native;
        /// @brief The FreeType library and per-face backend handles.
        Unique<Native> m_Native;
    };
}

VE_ENUM(::Veng::Text::GlyphFieldType, 0xE09CCF12D96986FCULL)
VE_ENUMERATOR(Msdf)
VE_ENUMERATOR(Sdf)
VE_ENUM_END();
