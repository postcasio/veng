#include "FlipbookImporter.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Path.h>
#include <Veng/Cook/JsonFile.h>

#include "TextureImporter.h"

namespace Veng::Cook
{
    namespace
    {
        // The schema version and file-kind discriminator this importer reads.
        constexpr i64 SchemaVersion = 1;
        constexpr const char* SchemaType = "flipbook-atlas";

        // Reads a positive integer field; a missing, non-integer, or non-positive value is an error.
        Result<u32> PositiveInt(const json& object, const char* key, const string& where)
        {
            if (!object.contains(key) || !object[key].is_number_integer() ||
                object[key].get<i64>() <= 0)
            {
                return std::unexpected(fmt::format(
                    "flipbook importer: '{}': '{}' must be a positive integer", where, key));
            }
            return static_cast<u32>(object[key].get<i64>());
        }

        // Reads a required string field; a missing or non-string value is an error.
        Result<string> RequiredString(const json& object, const char* key, const string& where)
        {
            if (!object.contains(key) || !object[key].is_string())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': missing or invalid '{}'", where, key));
            }
            return object[key].get<string>();
        }

        // Reads an optional [x, y, z] number triple into `out`; absent is not an error.
        Result<bool> OptionalTriple(const json& object, const char* key, f32 (&out)[3],
                                    const string& where)
        {
            if (!object.contains(key) || object[key].is_null())
            {
                return false;
            }
            const json& value = object[key];
            if (!value.is_array() || value.size() != 3 || !value[0].is_number() ||
                !value[1].is_number() || !value[2].is_number())
            {
                return std::unexpected(fmt::format(
                    "flipbook importer: '{}': '{}' must be an [x, y, z] number array", where, key));
            }
            for (usize axis = 0; axis < 3; ++axis)
            {
                out[axis] = value[axis].get<f32>();
            }
            return true;
        }
    }

    Result<vector<u8>> FlipbookImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("flipbook importer: missing or invalid 'source'");
        }
        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string where = sourcePath.string();

        const Result<json> manifestResult = ReadJsonFile(sourcePath, "flipbook importer");
        if (!manifestResult)
        {
            return std::unexpected(manifestResult.error());
        }
        const json& manifest = *manifestResult;

        if (!manifest.contains("version") || !manifest["version"].is_number_integer() ||
            manifest["version"].get<i64>() != SchemaVersion)
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': unsupported atlas schema version (expected {})", where,
                SchemaVersion));
        }
        if (!manifest.contains("type") || !manifest["type"].is_string() ||
            manifest["type"].get<string>() != SchemaType)
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': 'type' must be \"{}\"", where, SchemaType));
        }

        // A frame-sequence export carries one image per frame and no atlas; this asset plays a
        // single grid-packed sheet, so the manifest must name one.
        if (!manifest.contains("atlas") || !manifest["atlas"].is_object())
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': missing 'atlas' object", where));
        }
        const json& atlas = manifest["atlas"];
        if (!atlas.contains("file") || !atlas["file"].is_string() ||
            atlas["file"].get<string>().empty())
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': no 'atlas.file' — this is a frame-sequence export; "
                "export the effect as a single atlas image to cook it as a flipbook",
                where));
        }
        if (!manifest.contains("frame") || !manifest["frame"].is_object())
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': missing 'frame' object", where));
        }

        const Result<u32> atlasWidth = PositiveInt(atlas, "width", where);
        const Result<u32> atlasHeight = PositiveInt(atlas, "height", where);
        const Result<u32> frameWidth = PositiveInt(manifest["frame"], "width", where);
        const Result<u32> frameHeight = PositiveInt(manifest["frame"], "height", where);
        const Result<u32> columns = PositiveInt(manifest, "columns", where);
        const Result<u32> rows = PositiveInt(manifest, "rows", where);
        const Result<u32> frames = PositiveInt(manifest, "frames", where);
        for (const Result<u32>* field :
             {&atlasWidth, &atlasHeight, &frameWidth, &frameHeight, &columns, &rows, &frames})
        {
            if (!*field)
            {
                return std::unexpected(field->error());
            }
        }
        if (static_cast<u64>(*columns) * *rows < *frames)
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': {} frames do not fit a {}x{} grid", where,
                            *frames, *columns, *rows));
        }
        if (static_cast<u64>(*columns) * *frameWidth != *atlasWidth ||
            static_cast<u64>(*rows) * *frameHeight != *atlasHeight)
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': a {}x{} grid of {}x{} frames is not the {}x{} atlas",
                where, *columns, *rows, *frameWidth, *frameHeight, *atlasWidth, *atlasHeight));
        }

        if (!manifest.contains("fps") || !manifest["fps"].is_number() ||
            !(manifest["fps"].get<f64>() > 0.0))
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': 'fps' must be a positive number", where));
        }
        if (!manifest.contains("loop") || !manifest["loop"].is_boolean())
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': 'loop' must be a boolean", where));
        }

        const Result<string> blend = RequiredString(manifest, "blend", where);
        const Result<string> alpha = RequiredString(manifest, "alpha", where);
        const Result<string> colorSpace = RequiredString(manifest, "colorSpace", where);
        for (const Result<string>* field : {&blend, &alpha, &colorSpace})
        {
            if (!*field)
            {
                return std::unexpected(field->error());
            }
        }

        CookedFlipbookHeader header{};
        header.Version = CookedFlipbookVersion;
        header.Columns = *columns;
        header.Rows = *rows;
        header.FrameCount = *frames;
        header.FrameWidth = *frameWidth;
        header.FrameHeight = *frameHeight;
        header.Fps = manifest["fps"].get<f32>();
        header.Loop = manifest["loop"].get<bool>() ? 1u : 0u;

        if (*blend == "alpha")
        {
            header.Blend = 0;
        }
        else if (*blend == "additive")
        {
            header.Blend = 1;
        }
        else
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': unknown blend '{}' (expected \"alpha\" or \"additive\")",
                where, *blend));
        }

        if (*alpha == "premultiplied")
        {
            header.AlphaMode = 0;
        }
        else if (*alpha == "coverage")
        {
            header.AlphaMode = 1;
        }
        else if (*alpha == "luminance")
        {
            header.AlphaMode = 2;
        }
        else if (*alpha == "opaque")
        {
            header.AlphaMode = 3;
        }
        else
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': unknown alpha '{}' (expected \"premultiplied\", "
                "\"coverage\", \"luminance\", or \"opaque\")",
                where, *alpha));
        }

        if (*colorSpace != "srgb" && *colorSpace != "linear")
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': unknown colorSpace '{}' (expected \"srgb\" or "
                "\"linear\")",
                where, *colorSpace));
        }
        const bool srgb = *colorSpace == "srgb";

        const Result<bool> hasExtent =
            OptionalTriple(manifest, "worldExtent", header.WorldExtent, where);
        if (!hasExtent)
        {
            return std::unexpected(hasExtent.error());
        }
        header.HasWorldExtent = *hasExtent ? 1u : 0u;
        const Result<bool> hasPivot = OptionalTriple(manifest, "pivot", header.Pivot, where);
        if (!hasPivot)
        {
            return std::unexpected(hasPivot.error());
        }
        header.HasPivot = *hasPivot ? 1u : 0u;

        // The atlas cooks through the ordinary texture path. A linear sheet takes the Packed role
        // rather than Mask, since every configuration maps Packed to a full-channel codec and a
        // sprite needs all four. Clamp-to-edge keeps the outermost tiles from wrapping onto the
        // opposite edge under bilinear filtering.
        const json descriptor = {
            {"image", atlas["file"].get<string>()},
            {"srgb", srgb},
            {"role", srgb ? "Color" : "Packed"},
            {"sampler",
             {{"min", "linear"},
              {"mag", "linear"},
              {"mipmap", "linear"},
              {"wrap_u", "clamp_to_edge"},
              {"wrap_v", "clamp_to_edge"},
              {"wrap_w", "clamp_to_edge"}}},
        };
        const Result<vector<u8>> texture = CookTextureDescriptor(context, descriptor, sourcePath);
        if (!texture)
        {
            return std::unexpected(texture.error());
        }

        CookedTextureHeader textureHeader{};
        std::memcpy(&textureHeader, texture->data(), sizeof(textureHeader));
        if (textureHeader.Width != *atlasWidth || textureHeader.Height != *atlasHeight)
        {
            return std::unexpected(fmt::format(
                "flipbook importer: '{}': the atlas image is {}x{}, the manifest says {}x{}", where,
                textureHeader.Width, textureHeader.Height, *atlasWidth, *atlasHeight));
        }

        header.TextureBytes = static_cast<u32>(texture->size());
        vector<u8> blob(sizeof(header) + texture->size());
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), texture->data(), texture->size());
        return blob;
    }
}
