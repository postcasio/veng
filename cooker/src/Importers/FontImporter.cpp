#include "FontImporter.h"
#include <Veng/Asset/Path.h>

#include <cstring>
#include <map>
#include <vector>

#include <fmt/format.h>

#include <msdf-atlas-gen/msdf-atlas-gen.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>

namespace Veng::Cook
{
    namespace
    {
        // The MSDF em size (glyph height in atlas pixels) and pixel distance range the cook
        // defaults to when the source JSON omits them. 48px glyphs with a 4px range is the common
        // legible-at-small-sizes MSDF baseline.
        constexpr u32 DefaultGlyphSize = 48;
        constexpr f64 DefaultPixelRange = 4.0;

        // The edge-coloring angle threshold msdfgen recommends for MSDF corner classification.
        constexpr f64 EdgeColoringAngle = 3.0;

        // Builds the set of codepoints to cook: a named preset ("ascii" = printable ASCII,
        // "latin1" = printable Latin-1) plus any explicit codepoints in the "codepoints" array. The
        // space (0x20) is always included so a shaped run can advance across whitespace.
        Result<msdf_atlas::Charset> BuildCharset(const json& fontJson, const string& sourceLabel)
        {
            msdf_atlas::Charset charset;

            string preset = "ascii";
            if (fontJson.contains("charset") && fontJson["charset"].is_string())
            {
                preset = fontJson["charset"].get<string>();
            }

            if (preset == "ascii")
            {
                for (u32 cp = 0x20; cp <= 0x7E; cp++)
                {
                    charset.add(cp);
                }
            }
            else if (preset == "latin1")
            {
                for (u32 cp = 0x20; cp <= 0x7E; cp++)
                {
                    charset.add(cp);
                }
                for (u32 cp = 0xA0; cp <= 0xFF; cp++)
                {
                    charset.add(cp);
                }
            }
            else
            {
                return std::unexpected(
                    fmt::format("font importer: '{}': invalid charset '{}' (expected 'ascii' or "
                                "'latin1')",
                                sourceLabel, preset));
            }

            if (fontJson.contains("codepoints") && fontJson["codepoints"].is_array())
            {
                for (const json& entry : fontJson["codepoints"])
                {
                    if (entry.is_number_unsigned())
                    {
                        charset.add(entry.get<u32>());
                    }
                }
            }

            charset.add(0x20);
            return charset;
        }

        // Applies the source JSON's optional `variations` block onto a loaded face — an object of
        // axis name to design coordinate, `{"Weight": 700}`. A variable font carries a design space
        // rather than one weight, and FreeType hands back its default instance unless an axis is
        // set, so this block is what makes a second weight cookable from the same file. It must run
        // before the charset loads, because that is when the outlines are read.
        //
        // **The key is the axis's name as the font declares it, not its four-character tag** — the
        // name table string ("Weight", "Width", "Optical size"), which is the only identifier the
        // underlying call matches on. An unknown name is an error listing what the font does carry,
        // rather than a declaration silently ignored: a cook that quietly produced the default
        // instance is exactly the failure a second weight would be debugged through.
        optional<string> ApplyVariations(msdfgen::FreetypeHandle* freetype,
                                         msdfgen::FontHandle* font, const json& fontJson,
                                         const string& sourceLabel)
        {
            if (!fontJson.contains("variations"))
            {
                return std::nullopt;
            }
            if (!fontJson["variations"].is_object())
            {
                return fmt::format("font importer: '{}': 'variations' must be an object mapping an "
                                   "axis name to a coordinate",
                                   sourceLabel);
            }

            std::vector<msdfgen::FontVariationAxis> axes;
            if (!msdfgen::listFontVariationAxes(axes, freetype, font) || axes.empty())
            {
                return fmt::format("font importer: '{}': 'variations' was declared but the font "
                                   "carries no variation axes (it is not a variable font)",
                                   sourceLabel);
            }

            string available;
            for (const msdfgen::FontVariationAxis& axis : axes)
            {
                available +=
                    fmt::format("{}'{}' [{:g}, {:g}] default {:g}", available.empty() ? "" : ", ",
                                axis.name, axis.minValue, axis.maxValue, axis.defaultValue);
            }

            for (const auto& [name, value] : fontJson["variations"].items())
            {
                if (!value.is_number())
                {
                    return fmt::format("font importer: '{}': variation axis '{}' must be a number",
                                       sourceLabel, name);
                }
                if (!msdfgen::setFontVariationAxis(freetype, font, name.c_str(), value.get<f64>()))
                {
                    return fmt::format("font importer: '{}': the font declares no variation axis "
                                       "named '{}' (it carries {})",
                                       sourceLabel, name, available);
                }
            }
            return std::nullopt;
        }

        void AppendBytes(vector<u8>& blob, const void* data, usize size)
        {
            const usize offset = blob.size();
            blob.resize(offset + size);
            std::memcpy(blob.data() + offset, data, size);
        }
    }

    Result<vector<u8>> FontImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("font importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<json> fontJsonResult = ReadJsonFile(sourcePath, "font importer");
        if (!fontJsonResult)
        {
            return std::unexpected(fontJsonResult.error());
        }
        const json& fontJson = *fontJsonResult;

        if (!fontJson.contains("font") || !fontJson["font"].is_string())
        {
            return std::unexpected(
                fmt::format("font importer: '{}': missing or invalid 'font'", sourcePath.string()));
        }

        const path fontPath = sourcePath.parent_path() / fontJson["font"].get<string>();
        context.RecordDependency(fontPath);

        u32 glyphSize = DefaultGlyphSize;
        if (fontJson.contains("glyphSize") && fontJson["glyphSize"].is_number_unsigned())
        {
            glyphSize = fontJson["glyphSize"].get<u32>();
        }
        if (glyphSize == 0)
        {
            return std::unexpected(fmt::format("font importer: '{}': 'glyphSize' must be non-zero",
                                               sourcePath.string()));
        }

        f64 pixelRange = DefaultPixelRange;
        if (fontJson.contains("pixelRange") && fontJson["pixelRange"].is_number())
        {
            pixelRange = fontJson["pixelRange"].get<f64>();
        }

        const Result<msdf_atlas::Charset> charset = BuildCharset(fontJson, sourcePath.string());
        if (!charset)
        {
            return std::unexpected(charset.error());
        }

        // FreeType loads the font's outlines; the handles are freed on every exit path below.
        msdfgen::FreetypeHandle* freetype = msdfgen::initializeFreetype();
        if (freetype == nullptr)
        {
            return std::unexpected(fmt::format("font importer: '{}': failed to initialize FreeType",
                                               sourcePath.string()));
        }

        msdfgen::FontHandle* font = msdfgen::loadFont(freetype, fontPath.string().c_str());
        if (font == nullptr)
        {
            msdfgen::deinitializeFreetype(freetype);
            return std::unexpected(fmt::format("font importer: '{}': failed to load font '{}'",
                                               sourcePath.string(), fontPath.string()));
        }

        if (const optional<string> failure =
                ApplyVariations(freetype, font, fontJson, sourcePath.string());
            failure.has_value())
        {
            msdfgen::destroyFont(font);
            msdfgen::deinitializeFreetype(freetype);
            return std::unexpected(*failure);
        }

        // fontScale 1.0 normalizes every glyph geometry, advance, and kerning value to em units
        // (the em becomes 1.0), so the cooked metrics scale to pixels by a single runtime multiply.
        // Kerning comes from the font's legacy `kern` table (a GPOS-only font cooks none).
        std::vector<msdf_atlas::GlyphGeometry> glyphStorage;
        msdf_atlas::FontGeometry fontGeometry(&glyphStorage);
        const int loaded = fontGeometry.loadCharset(font, 1.0, *charset);

        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(freetype);

        if (loaded <= 0)
        {
            return std::unexpected(fmt::format("font importer: '{}': loaded no glyphs from '{}'",
                                               sourcePath.string(), fontPath.string()));
        }

        // Edge-color each glyph's shape so the three MSDF channels encode the corner geometry.
        for (msdf_atlas::GlyphGeometry& glyph : glyphStorage)
        {
            glyph.edgeColoring(&msdfgen::edgeColoringSimple, EdgeColoringAngle, 0);
        }

        // Pack the glyphs into a square, power-of-two atlas at the requested em size, with the
        // pixel distance range baked in. The packer solves the tightest layout at that fixed scale.
        msdf_atlas::TightAtlasPacker packer;
        packer.setDimensionsConstraint(msdf_atlas::DimensionsConstraint::POWER_OF_TWO_SQUARE);
        packer.setScale(static_cast<f64>(glyphSize));
        packer.setPixelRange(msdfgen::Range(pixelRange));
        packer.setMiterLimit(1.0);
        packer.setSpacing(1);
        if (packer.pack(glyphStorage.data(), static_cast<int>(glyphStorage.size())) != 0)
        {
            return std::unexpected(fmt::format("font importer: '{}': failed to pack glyph atlas",
                                               sourcePath.string()));
        }

        int atlasWidth = 0;
        int atlasHeight = 0;
        packer.getDimensions(atlasWidth, atlasHeight);
        if (atlasWidth <= 0 || atlasHeight <= 0)
        {
            return std::unexpected(fmt::format("font importer: '{}': packed atlas has zero size",
                                               sourcePath.string()));
        }

        // Render the three-channel MSDF into a float bitmap, then quantize each channel to 8-bit
        // (the atlas is stored uncompressed RGBA8; alpha is opaque). The generator runs the
        // packed layout the packer computed.
        msdf_atlas::ImmediateAtlasGenerator<float, 3, msdf_atlas::msdfGenerator,
                                            msdf_atlas::BitmapAtlasStorage<float, 3>>
            generator(atlasWidth, atlasHeight);
        generator.setAttributes(msdf_atlas::GeneratorAttributes{});
        generator.setThreadCount(1);
        generator.generate(glyphStorage.data(), static_cast<int>(glyphStorage.size()));

        const msdfgen::BitmapConstRef<float, 3> bitmap = generator.atlasStorage();

        const usize atlasTexels = static_cast<usize>(atlasWidth) * atlasHeight;
        vector<u8> atlasPixels(atlasTexels * 4, 255);
        for (int y = 0; y < atlasHeight; y++)
        {
            for (int x = 0; x < atlasWidth; x++)
            {
                const float* src = bitmap(x, y);
                // The float bitmap is bottom-up (msdfgen's origin); store top-down to match the
                // atlas rects the cook records (top-left origin) and the runtime's row-major upload.
                const usize dst = (static_cast<usize>(atlasHeight - 1 - y) * atlasWidth + x) * 4;
                for (int c = 0; c < 3; c++)
                {
                    const float clamped = src[c] < 0.0f ? 0.0f : (src[c] > 1.0f ? 1.0f : src[c]);
                    atlasPixels[dst + c] = static_cast<u8>(clamped * 255.0f + 0.5f);
                }
            }
        }

        // Map each cooked glyph's msdfgen glyph index back to its codepoint so the kerning table
        // (keyed by index) can be re-expressed in codepoints — the runtime shapes by codepoint.
        std::map<int, u32> indexToCodepoint;

        vector<CookedGlyph> cookedGlyphs;
        cookedGlyphs.reserve(glyphStorage.size());
        for (const msdf_atlas::GlyphGeometry& glyph : glyphStorage)
        {
            const u32 codepoint = glyph.getCodepoint();
            if (codepoint == 0)
            {
                continue;
            }
            indexToCodepoint[glyph.getIndex()] = codepoint;

            double planeLeft = 0.0;
            double planeBottom = 0.0;
            double planeRight = 0.0;
            double planeTop = 0.0;
            glyph.getQuadPlaneBounds(planeLeft, planeBottom, planeRight, planeTop);

            double atlasLeft = 0.0;
            double atlasBottom = 0.0;
            double atlasRight = 0.0;
            double atlasTop = 0.0;
            glyph.getQuadAtlasBounds(atlasLeft, atlasBottom, atlasRight, atlasTop);

            CookedGlyph cooked{};
            cooked.Codepoint = codepoint;
            cooked.Advance = static_cast<f32>(glyph.getAdvance());
            cooked.PlaneLeft = static_cast<f32>(planeLeft);
            cooked.PlaneBottom = static_cast<f32>(planeBottom);
            cooked.PlaneWidth = static_cast<f32>(planeRight - planeLeft);
            cooked.PlaneHeight = static_cast<f32>(planeTop - planeBottom);
            // msdfgen atlas bounds are bottom-up; convert to the top-down atlas rect the runtime
            // reads (top-left origin), so AtlasTop is the rect's top edge in the stored image.
            cooked.AtlasLeft = static_cast<f32>(atlasLeft);
            cooked.AtlasTop = static_cast<f32>(static_cast<double>(atlasHeight) - atlasTop);
            cooked.AtlasWidth = static_cast<f32>(atlasRight - atlasLeft);
            cooked.AtlasHeight = static_cast<f32>(atlasTop - atlasBottom);
            cookedGlyphs.push_back(cooked);
        }

        vector<CookedKernPair> cookedKerning;
        for (const auto& [pair, advance] : fontGeometry.getKerning())
        {
            const auto leftIt = indexToCodepoint.find(pair.first);
            const auto rightIt = indexToCodepoint.find(pair.second);
            if (leftIt == indexToCodepoint.end() || rightIt == indexToCodepoint.end())
            {
                continue;
            }
            cookedKerning.push_back(CookedKernPair{
                .Left = leftIt->second,
                .Right = rightIt->second,
                .Advance = static_cast<f32>(advance),
            });
        }

        const msdfgen::FontMetrics& metrics = fontGeometry.getMetrics();

        CookedFontHeader header{};
        header.Version = CookedFontVersion;
        header.AtlasWidth = static_cast<u32>(atlasWidth);
        header.AtlasHeight = static_cast<u32>(atlasHeight);
        header.AtlasFormat = 2u; // RGBA8Unorm (Renderer::Format ordinal)
        header.DistanceRange = static_cast<f32>(pixelRange);
        header.EmSize = static_cast<f32>(metrics.emSize);
        header.LineHeight = static_cast<f32>(metrics.lineHeight);
        header.Ascender = static_cast<f32>(metrics.ascenderY);
        header.Descender = static_cast<f32>(metrics.descenderY);
        header.GlyphCount = static_cast<u32>(cookedGlyphs.size());
        header.KerningCount = static_cast<u32>(cookedKerning.size());

        vector<u8> blob;
        blob.reserve(sizeof(header) + cookedGlyphs.size() * sizeof(CookedGlyph) +
                     cookedKerning.size() * sizeof(CookedKernPair) + atlasPixels.size());
        AppendBytes(blob, &header, sizeof(header));
        if (!cookedGlyphs.empty())
        {
            AppendBytes(blob, cookedGlyphs.data(), cookedGlyphs.size() * sizeof(CookedGlyph));
        }
        if (!cookedKerning.empty())
        {
            AppendBytes(blob, cookedKerning.data(), cookedKerning.size() * sizeof(CookedKernPair));
        }
        AppendBytes(blob, atlasPixels.data(), atlasPixels.size());

        return blob;
    }
}
