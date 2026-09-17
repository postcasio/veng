#include <Veng/Text/GlyphSource.h>

#include <Veng/Assert.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// FreeType must precede the msdfgen extension header: import-font.h gates its FT_Face-taking
// helpers (adoptFreetypeFont) on FT_LOAD_DEFAULT being defined.
#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/ftmodapi.h>

#include <msdfgen.h>
#include <msdfgen-ext.h>

namespace Veng::Text
{
    namespace
    {
        // The edge-coloring angle threshold msdfgen recommends for MSDF corner classification —
        // the same value the offline atlas cook uses, so a runtime-rasterized MSDF glyph matches
        // the field an offline bake of the same outline would produce.
        constexpr f64 EdgeColoringAngle = 3.0;

        // FreeType's SDF renderer clamps its spread (the signed-distance half-range, in pixels) to
        // this closed interval; the requested distance range is clamped into it before it is set.
        constexpr u32 MinSdfSpread = 2;
        constexpr u32 MaxSdfSpread = 32;

        // Quantizes an f32 signed-distance sample in [0, 1] to a byte, matching the offline cook's
        // clamp-and-round.
        u8 FieldByte(float value)
        {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<u8>(clamped * 255.0f + 0.5f);
        }
    }

    struct GlyphSource::Native
    {
        // One face the source has loaded: the FreeType face over the caller's borrowed bytes, and
        // an msdfgen wrapper adopting that same face for the MSDF path. adoptFreetypeFont does not
        // own the FT_Face, so destroyFont leaves it for FT_Done_Face to release.
        struct Face
        {
            FT_Face FtFace = nullptr;
            msdfgen::FontHandle* MsdfFont = nullptr;
            FaceMetrics Metrics;
        };

        FT_Library Library = nullptr;
        std::vector<Face> Faces;
    };

    GlyphSource::GlyphSource() : m_Native(std::make_unique<Native>())
    {
        const FT_Error error = FT_Init_FreeType(&m_Native->Library);
        VE_ASSERT(error == 0, "GlyphSource: FT_Init_FreeType failed");
    }

    GlyphSource::~GlyphSource()
    {
        for (const Native::Face& face : m_Native->Faces)
        {
            if (face.MsdfFont != nullptr)
            {
                msdfgen::destroyFont(face.MsdfFont);
            }
            if (face.FtFace != nullptr)
            {
                FT_Done_Face(face.FtFace);
            }
        }
        if (m_Native->Library != nullptr)
        {
            FT_Done_FreeType(m_Native->Library);
        }
    }

    Result<FaceId> GlyphSource::LoadFace(std::span<const std::byte> fontData)
    {
        if (fontData.empty())
        {
            return std::unexpected("GlyphSource: cannot load a face from empty font data");
        }

        FT_Face ftFace = nullptr;
        const FT_Error error =
            FT_New_Memory_Face(m_Native->Library, reinterpret_cast<const FT_Byte*>(fontData.data()),
                               static_cast<FT_Long>(fontData.size()), 0, &ftFace);
        if (error != 0 || ftFace == nullptr)
        {
            return std::unexpected("GlyphSource: font data is not a loadable face");
        }
        if (ftFace->units_per_EM == 0)
        {
            FT_Done_Face(ftFace);
            return std::unexpected(
                "GlyphSource: face has no em square (bitmap-only fonts are unsupported)");
        }

        msdfgen::FontHandle* msdfFont = msdfgen::adoptFreetypeFont(ftFace);
        if (msdfFont == nullptr)
        {
            FT_Done_Face(ftFace);
            return std::unexpected("GlyphSource: failed to adopt the face for MSDF generation");
        }

        const f32 unitsPerEm = static_cast<f32>(ftFace->units_per_EM);
        Native::Face face;
        face.FtFace = ftFace;
        face.MsdfFont = msdfFont;
        face.Metrics.UnitsPerEm = unitsPerEm;
        face.Metrics.Ascender = static_cast<f32>(ftFace->ascender) / unitsPerEm;
        face.Metrics.Descender = static_cast<f32>(ftFace->descender) / unitsPerEm;
        face.Metrics.LineHeight = static_cast<f32>(ftFace->height) / unitsPerEm;

        const auto id = static_cast<u32>(m_Native->Faces.size());
        m_Native->Faces.push_back(face);
        return static_cast<FaceId>(id);
    }

    FaceMetrics GlyphSource::GetFaceMetrics(FaceId face) const
    {
        const auto index = static_cast<usize>(face);
        VE_ASSERT(index < m_Native->Faces.size(), "GlyphSource: invalid FaceId");
        return m_Native->Faces[index].Metrics;
    }

    bool GlyphSource::HasGlyph(FaceId face, u32 codepoint) const
    {
        return GlyphIndex(face, codepoint) != 0;
    }

    u32 GlyphSource::GlyphIndex(FaceId face, u32 codepoint) const
    {
        const auto index = static_cast<usize>(face);
        VE_ASSERT(index < m_Native->Faces.size(), "GlyphSource: invalid FaceId");
        return FT_Get_Char_Index(m_Native->Faces[index].FtFace, codepoint);
    }

    f32 GlyphSource::GetKerning(FaceId face, u32 leftIndex, u32 rightIndex) const
    {
        const auto index = static_cast<usize>(face);
        VE_ASSERT(index < m_Native->Faces.size(), "GlyphSource: invalid FaceId");
        const Native::Face& entry = m_Native->Faces[index];

        FT_Vector kerning{};
        // FT_KERNING_UNSCALED reads the legacy `kern` table in font units; a GPOS-only face carries
        // none and returns zero. Em-normalize to match the advance/plane-bounds convention.
        if (FT_Get_Kerning(entry.FtFace, leftIndex, rightIndex, FT_KERNING_UNSCALED, &kerning) != 0)
        {
            return 0.0f;
        }
        return static_cast<f32>(kerning.x) / entry.Metrics.UnitsPerEm;
    }

    Result<RasterizedGlyph> GlyphSource::Rasterize(FaceId face, u32 glyphIndex, f32 pixelSize,
                                                   GlyphFieldType fieldType, u32 distanceRangePx)
    {
        const auto index = static_cast<usize>(face);
        VE_ASSERT(index < m_Native->Faces.size(), "GlyphSource: invalid FaceId");
        if (!(pixelSize > 0.0f))
        {
            return std::unexpected("GlyphSource: pixel size must be positive");
        }
        const Native::Face& entry = m_Native->Faces[index];

        // The advance is a face property, size-independent — read it in font units and em-normalize,
        // so both field paths report the identical advance for a glyph.
        if (FT_Load_Glyph(entry.FtFace, glyphIndex, FT_LOAD_NO_SCALE) != 0)
        {
            return std::unexpected("GlyphSource: failed to load glyph outline");
        }
        const f32 advance =
            static_cast<f32>(entry.FtFace->glyph->advance.x) / entry.Metrics.UnitsPerEm;

        RasterizedGlyph out;
        out.FieldType = fieldType;
        out.Advance = advance;

        if (fieldType == GlyphFieldType::Msdf)
        {
            out.Channels = 4;

            msdfgen::Shape shape;
            if (!msdfgen::loadGlyph(shape, entry.MsdfFont, msdfgen::GlyphIndex(glyphIndex),
                                    msdfgen::FONT_SCALING_EM_NORMALIZED))
            {
                return std::unexpected("GlyphSource: failed to load glyph shape for MSDF");
            }
            shape.normalize();

            // A whitespace glyph has no contour: zero-size bitmap, advance-only.
            if (shape.contours.empty())
            {
                return out;
            }

            msdfgen::edgeColoringSimple(shape, EdgeColoringAngle, 0);

            // The signed-distance range in em units. The glyph quad is the outline's bounds grown
            // by half the range on every side, the margin the field transitions across, so the
            // whole field fits the bitmap. Plane bounds are em-space and y-up (baseline at 0).
            const f64 rangeEm = static_cast<f64>(distanceRangePx) / static_cast<f64>(pixelSize);
            const msdfgen::Shape::Bounds bounds = shape.getBounds();
            const f64 left = bounds.l - 0.5 * rangeEm;
            const f64 bottom = bounds.b - 0.5 * rangeEm;
            const f64 right = bounds.r + 0.5 * rangeEm;
            const f64 top = bounds.t + 0.5 * rangeEm;

            const auto width =
                static_cast<u32>(std::ceil((right - left) * static_cast<f64>(pixelSize)));
            const auto height =
                static_cast<u32>(std::ceil((top - bottom) * static_cast<f64>(pixelSize)));
            if (width == 0 || height == 0)
            {
                return out;
            }

            msdfgen::Bitmap<float, 3> field(static_cast<int>(width), static_cast<int>(height));
            msdfgen::generateMSDF(
                field, shape, msdfgen::Range(rangeEm),
                msdfgen::Vector2(static_cast<f64>(pixelSize), static_cast<f64>(pixelSize)),
                msdfgen::Vector2(-left, -bottom));

            out.Width = width;
            out.Height = height;
            out.PlaneMin = vec2{static_cast<f32>(left), static_cast<f32>(bottom)};
            out.PlaneMax = vec2{static_cast<f32>(right), static_cast<f32>(top)};
            out.Pixels.resize(static_cast<usize>(width) * height * 4, 255);
            for (u32 y = 0; y < height; y++)
            {
                // msdfgen's bitmap origin is bottom-left; store rows top-down for the atlas upload.
                const int srcY = static_cast<int>(height - 1 - y);
                for (u32 x = 0; x < width; x++)
                {
                    const float* src = field(static_cast<int>(x), srcY);
                    const usize dst = (static_cast<usize>(y) * width + x) * 4;
                    out.Pixels[dst + 0] = FieldByte(src[0]);
                    out.Pixels[dst + 1] = FieldByte(src[1]);
                    out.Pixels[dst + 2] = FieldByte(src[2]);
                    out.Pixels[dst + 3] = 255;
                }
            }
            return out;
        }

        // Sdf: FreeType renders a single-channel true SDF straight from the outline. Its spread is
        // the same distance range the MSDF path encodes, so one shader range constant serves both.
        out.Channels = 1;

        const auto pixelsInt = static_cast<FT_UInt>(std::max(1.0f, std::round(pixelSize)));
        if (FT_Set_Pixel_Sizes(entry.FtFace, 0, pixelsInt) != 0)
        {
            return std::unexpected("GlyphSource: failed to set pixel size for SDF");
        }

        const auto spread =
            static_cast<FT_Int>(std::clamp(distanceRangePx, MinSdfSpread, MaxSdfSpread));
        FT_Property_Set(m_Native->Library, "sdf", "spread", &spread);

        if (FT_Load_Glyph(entry.FtFace, glyphIndex, FT_LOAD_DEFAULT) != 0)
        {
            return std::unexpected("GlyphSource: failed to load glyph for SDF");
        }
        FT_GlyphSlot slot = entry.FtFace->glyph;

        // A whitespace glyph has no outline: zero-size bitmap, advance-only.
        if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_contours == 0)
        {
            return out;
        }

        if (FT_Render_Glyph(slot, FT_RENDER_MODE_SDF) != 0)
        {
            return std::unexpected("GlyphSource: FT_Render_Glyph (SDF) failed");
        }

        const FT_Bitmap& bitmap = slot->bitmap;
        const u32 width = bitmap.width;
        const u32 height = bitmap.rows;
        const f32 pixelsF = static_cast<f32>(pixelsInt);
        // bitmap_left/top already include the spread border, in pixels from the pen origin (y-up).
        out.PlaneMin =
            vec2{static_cast<f32>(slot->bitmap_left) / pixelsF,
                 static_cast<f32>(slot->bitmap_top - static_cast<int>(height)) / pixelsF};
        out.PlaneMax = vec2{static_cast<f32>(slot->bitmap_left + static_cast<int>(width)) / pixelsF,
                            static_cast<f32>(slot->bitmap_top) / pixelsF};

        if (width == 0 || height == 0)
        {
            return out;
        }

        out.Width = width;
        out.Height = height;
        out.Pixels.resize(static_cast<usize>(width) * height);
        // FT's SDF output is a down-flow single-channel (GRAY) bitmap, so row y lives at
        // buffer + y * pitch and copies straight into the top-down destination.
        for (u32 y = 0; y < height; y++)
        {
            const u8* row = bitmap.buffer + static_cast<isize>(y) * bitmap.pitch;
            std::memcpy(out.Pixels.data() + static_cast<usize>(y) * width, row, width);
        }
        return out;
    }
}
