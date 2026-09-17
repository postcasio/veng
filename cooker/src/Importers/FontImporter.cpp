#include "FontImporter.h"
#include <Veng/Asset/Path.h>

#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include <fmt/format.h>

#include <msdf-atlas-gen/msdf-atlas-gen.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/JsonFile.h>

namespace Veng::Cook
{
    namespace
    {
        // The curated General-Punctuation marks real UI text uses that live outside Latin-1: the
        // "latin-extended" preset carries them so a translated string renders proper typographic
        // punctuation rather than the tofu box. Not the whole block — each glyph is atlas area.
        constexpr std::array<u32, 9> GeneralPunctuation = {
            0x2010, // hyphen
            0x2011, // non-breaking hyphen
            0x2013, // en dash
            0x2014, // em dash
            0x2018, // left single quote
            0x2019, // right single quote
            0x201C, // left double quote
            0x201D, // right double quote
            0x2026, // horizontal ellipsis
        };

        // Builds the set of codepoints to cook: a named preset ("ascii" = printable ASCII,
        // "latin1" = printable Latin-1, "latin-extended" = Latin-1 + Latin Extended-A + curated
        // typographic punctuation) plus any explicit codepoints in the "codepoints" array. The
        // space (0x20) is always included so a shaped run can advance across whitespace.
        Result<msdf_atlas::Charset> BuildCharset(const json& fontJson, const string& sourceLabel)
        {
            msdf_atlas::Charset charset;

            // The preset names the hot set: the codepoints the cook bakes into the atlas and the
            // runtime pre-rasterizes at load. "hotset" is the current key; "charset" is the earlier
            // spelling of the same selector, still accepted.
            string preset = "ascii";
            if (fontJson.contains("hotset") && fontJson["hotset"].is_string())
            {
                preset = fontJson["hotset"].get<string>();
            }
            else if (fontJson.contains("charset") && fontJson["charset"].is_string())
            {
                preset = fontJson["charset"].get<string>();
            }

            const auto addAscii = [&charset]()
            {
                for (u32 cp = 0x20; cp <= 0x7E; cp++)
                {
                    charset.add(cp);
                }
            };
            const auto addLatin1Supplement = [&charset]()
            {
                for (u32 cp = 0xA0; cp <= 0xFF; cp++)
                {
                    charset.add(cp);
                }
            };

            if (preset == "ascii")
            {
                addAscii();
            }
            else if (preset == "latin1")
            {
                addAscii();
                addLatin1Supplement();
            }
            else if (preset == "latin-extended")
            {
                addAscii();
                addLatin1Supplement();
                // Latin Extended-A: the Central/Eastern-European and Turkish letters (ą ć ę ł, č ř
                // š ž, ő ű, ğ ı İ ş, …).
                for (u32 cp = 0x100; cp <= 0x17F; cp++)
                {
                    charset.add(cp);
                }
                for (const u32 cp : GeneralPunctuation)
                {
                    charset.add(cp);
                }
            }
            else
            {
                return std::unexpected(
                    fmt::format("font importer: '{}': invalid charset '{}' (expected 'ascii', "
                                "'latin1', or 'latin-extended')",
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

        // The runtime rasterizes any covered codepoint from the face itself, so the file's bytes are
        // embedded rather than consumed and discarded — the runtime GlyphSource loads them from
        // memory. The same bytes still feed the offline atlas bake below.
        vector<u8> faceBytes;
        {
            std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
            if (!file.good())
            {
                return std::unexpected(fmt::format("font importer: '{}': cannot open font '{}'",
                                                   sourcePath.string(), fontPath.string()));
            }
            const std::streamsize size = file.tellg();
            file.seekg(0);
            faceBytes.resize(static_cast<usize>(size));
            if (size > 0)
            {
                file.read(reinterpret_cast<char*>(faceBytes.data()), size);
            }
            if (!file.good() || faceBytes.empty())
            {
                return std::unexpected(fmt::format("font importer: '{}': failed to read font '{}'",
                                                   sourcePath.string(), fontPath.string()));
            }
        }

        // The default glyph field type this font rasterizes into at runtime: msdf (crisp, multi-
        // channel) or sdf (cheap, single-channel). The offline atlas below is always MSDF.
        u32 fieldType = 0;
        if (fontJson.contains("field"))
        {
            if (!fontJson["field"].is_string())
            {
                return std::unexpected(
                    fmt::format("font importer: '{}': 'field' must be a string ('msdf' or 'sdf')",
                                sourcePath.string()));
            }
            const string field = fontJson["field"].get<string>();
            if (field == "msdf")
            {
                fieldType = 0;
            }
            else if (field == "sdf")
            {
                fieldType = 1;
            }
            else
            {
                return std::unexpected(fmt::format(
                    "font importer: '{}': invalid field '{}' (expected 'msdf' or 'sdf')",
                    sourcePath.string(), field));
            }
        }

        // The fallback chain: other Font assets whose faces cover what this one does not, in
        // priority order. Each id must resolve to a Font, the same reference discipline a material's
        // texture takes.
        vector<u64> fallbackIds;
        if (fontJson.contains("fallback"))
        {
            if (!fontJson["fallback"].is_array())
            {
                return std::unexpected(fmt::format(
                    "font importer: '{}': 'fallback' must be an array of font id strings",
                    sourcePath.string()));
            }
            for (const json& item : fontJson["fallback"])
            {
                if (!item.is_string())
                {
                    return std::unexpected(fmt::format(
                        "font importer: '{}': each 'fallback' entry must be a hex font id string",
                        sourcePath.string()));
                }
                const optional<AssetId> fallback = ParseAssetId(item.get<string>());
                if (!fallback)
                {
                    return std::unexpected(
                        fmt::format("font importer: '{}': fallback '{}' is not a valid hex id",
                                    sourcePath.string(), item.get<string>()));
                }
                if (context.Resolve)
                {
                    const optional<ResolvedSource> resolved = context.Resolve(*fallback);
                    if (!resolved)
                    {
                        return std::unexpected(fmt::format(
                            "font importer: '{}': fallback {} not found in pack or reference packs",
                            sourcePath.string(), item.get<string>()));
                    }
                    if (resolved->Type != AssetTypes::Font)
                    {
                        return std::unexpected(
                            fmt::format("font importer: '{}': fallback {} is not a Font asset",
                                        sourcePath.string(), item.get<string>()));
                    }
                }
                fallbackIds.push_back(fallback->Value);
            }
        }

        const Result<msdf_atlas::Charset> charset = BuildCharset(fontJson, sourcePath.string());
        if (!charset)
        {
            return std::unexpected(charset.error());
        }

        // The hot set the runtime pre-rasterizes into the shared atlas at load is exactly the charset
        // cooked into the atlas below.
        vector<u32> hotsetCodepoints(charset->begin(), charset->end());

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

        // fontScale 1.0 normalizes every glyph geometry and advance to em units (the em becomes
        // 1.0), so the cooked line metrics scale to pixels by a single runtime multiply. Loading the
        // charset is what populates the font's global metrics; the outlines themselves are not baked
        // — the runtime rasterizes every glyph on demand from the embedded face bytes — but the load
        // still confirms the hot-set codepoints resolve to real glyphs.
        std::vector<msdf_atlas::GlyphGeometry> glyphStorage;
        msdf_atlas::FontGeometry fontGeometry(&glyphStorage);
        const int loaded = fontGeometry.loadCharset(font, 1.0, *charset);
        const msdfgen::FontMetrics metrics = fontGeometry.getMetrics();

        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(freetype);

        if (loaded <= 0)
        {
            return std::unexpected(fmt::format("font importer: '{}': loaded no glyphs from '{}'",
                                               sourcePath.string(), fontPath.string()));
        }

        CookedFontHeader header{};
        header.Version = CookedFontVersion;
        header.EmSize = static_cast<f32>(metrics.emSize);
        header.LineHeight = static_cast<f32>(metrics.lineHeight);
        header.Ascender = static_cast<f32>(metrics.ascenderY);
        header.Descender = static_cast<f32>(metrics.descenderY);
        header.FieldType = fieldType;
        header.FaceBytes = static_cast<u32>(faceBytes.size());
        header.HotsetCount = static_cast<u32>(hotsetCodepoints.size());
        header.FallbackCount = static_cast<u32>(fallbackIds.size());

        vector<u8> blob;
        blob.reserve(sizeof(header) + faceBytes.size() + hotsetCodepoints.size() * sizeof(u32) +
                     fallbackIds.size() * sizeof(u64));
        AppendBytes(blob, &header, sizeof(header));
        AppendBytes(blob, faceBytes.data(), faceBytes.size());
        if (!hotsetCodepoints.empty())
        {
            AppendBytes(blob, hotsetCodepoints.data(), hotsetCodepoints.size() * sizeof(u32));
        }
        if (!fallbackIds.empty())
        {
            AppendBytes(blob, fallbackIds.data(), fallbackIds.size() * sizeof(u64));
        }

        return blob;
    }
}
