#include "FlipbookImporter.h"

#include <algorithm>
#include <array>
#include <cmath>
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
        // The file-kind discriminator this importer reads, and the schema versions it accepts.
        constexpr const char* SchemaType = "flipbook-atlas";
        constexpr i64 SchemaVersionOne = 1;
        constexpr i64 SchemaVersionTwo = 2;

        // BC7 and ASTC alpha err by up to 10 and 8 units on a sqrt-temperature front, BC4 by 3.
        constexpr bool SplitTemperature = true;

        // The cooked texture slots (CookedFlipbookHeader::TextureBytes).
        constexpr u32 PrimarySlot = 0;
        constexpr u32 LightBSlot = 1;
        constexpr u32 MotionSlot = 2;
        constexpr u32 TemperatureSlot = 3;

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

        // Reads a "srgb" | "linear" colour space; true for sRGB.
        Result<bool> ParseColorSpace(const string& value, const string& field, const string& where)
        {
            if (value != "srgb" && value != "linear")
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': unknown {} '{}' (expected \"srgb\" or "
                                "\"linear\")",
                                where, field, value));
            }
            return value == "srgb";
        }

        // The grid and timing every schema version shares, read into `header`.
        struct Grid
        {
            u32 Width = 0;
            u32 Height = 0;
        };

        // Reads the grid (`frame`, `columns`, `rows`, `frames`) and the timing (`fps`, `loop`).
        // The atlas's own size is checked against the grid by the caller.
        Result<Grid> ReadGridAndTiming(const json& manifest, CookedFlipbookHeader& header,
                                       const string& where)
        {
            if (!manifest.contains("frame") || !manifest["frame"].is_object())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': missing 'frame' object", where));
            }
            const Result<u32> frameWidth = PositiveInt(manifest["frame"], "width", where);
            const Result<u32> frameHeight = PositiveInt(manifest["frame"], "height", where);
            const Result<u32> columns = PositiveInt(manifest, "columns", where);
            const Result<u32> rows = PositiveInt(manifest, "rows", where);
            const Result<u32> frames = PositiveInt(manifest, "frames", where);
            for (const Result<u32>* field : {&frameWidth, &frameHeight, &columns, &rows, &frames})
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

            header.Columns = *columns;
            header.Rows = *rows;
            header.FrameCount = *frames;
            header.FrameWidth = *frameWidth;
            header.FrameHeight = *frameHeight;
            header.Fps = manifest["fps"].get<f32>();
            header.Loop = manifest["loop"].get<bool>() ? 1u : 0u;
            return Grid{.Width = *columns * *frameWidth, .Height = *rows * *frameHeight};
        }

        // Reads the recommended `blend`.
        VoidResult ReadBlend(const json& manifest, CookedFlipbookHeader& header,
                             const string& where)
        {
            const Result<string> blend = RequiredString(manifest, "blend", where);
            if (!blend)
            {
                return std::unexpected(blend.error());
            }
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
                return std::unexpected(fmt::format("flipbook importer: '{}': unknown blend '{}' "
                                                   "(expected \"alpha\" or \"additive\")",
                                                   where, *blend));
            }
            return {};
        }

        // Reads the optional `worldExtent` and `pivot`.
        VoidResult ReadPlacement(const json& manifest, CookedFlipbookHeader& header,
                                 const string& where)
        {
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
            return {};
        }

        // One texture to cook into a slot: the image, how its values are stored, and its role.
        struct SlotCook
        {
            string File;
            bool Srgb = false;
            const char* Role = "Packed";
            const char* Alpha = "straight";
            const char* SourceChannel = nullptr;
            // The name an error reports the texture by.
            string Label;
        };

        // Cooks one slot's texture through the ordinary texture path and checks its size against
        // the grid. Clamp-to-edge keeps the outermost tiles from wrapping onto the opposite edge
        // under bilinear filtering.
        Result<vector<u8>> CookSlot(const CookContext& context, const path& sourcePath,
                                    const SlotCook& slot, const Grid& grid, const string& where)
        {
            json descriptor = {
                {"image", slot.File},
                {"srgb", slot.Srgb},
                {"role", slot.Role},
                {"alpha", slot.Alpha},
                {"sampler",
                 {{"min", "linear"},
                  {"mag", "linear"},
                  {"mipmap", "linear"},
                  {"wrap_u", "clamp_to_edge"},
                  {"wrap_v", "clamp_to_edge"},
                  {"wrap_w", "clamp_to_edge"}}},
            };
            if (slot.SourceChannel != nullptr)
            {
                descriptor["source_channel"] = slot.SourceChannel;
            }
            Result<vector<u8>> texture = CookTextureDescriptor(context, descriptor, sourcePath);
            if (!texture)
            {
                return std::unexpected(texture.error());
            }

            CookedTextureHeader textureHeader{};
            std::memcpy(&textureHeader, texture->data(), sizeof(textureHeader));
            if (textureHeader.Width != grid.Width || textureHeader.Height != grid.Height)
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': the {} image is {}x{}, the manifest "
                                "says {}x{}",
                                where, slot.Label, textureHeader.Width, textureHeader.Height,
                                grid.Width, grid.Height));
            }
            return texture;
        }

        // Concatenates the header, the per-frame scales and the present slots into the blob.
        vector<u8> AssembleBlob(CookedFlipbookHeader header, const vector<f32>& frameScale,
                                const std::array<vector<u8>, CookedFlipbookTextureSlots>& slots)
        {
            usize size = sizeof(header) + frameScale.size() * sizeof(f32);
            for (u32 slot = 0; slot < CookedFlipbookTextureSlots; ++slot)
            {
                header.TextureBytes[slot] = static_cast<u32>(slots[slot].size());
                size += slots[slot].size();
            }
            header.HasFrameScale = frameScale.empty() ? 0u : 1u;

            vector<u8> blob(size);
            usize offset = 0;
            std::memcpy(blob.data(), &header, sizeof(header));
            offset += sizeof(header);
            if (!frameScale.empty())
            {
                std::memcpy(blob.data() + offset, frameScale.data(),
                            frameScale.size() * sizeof(f32));
                offset += frameScale.size() * sizeof(f32);
            }
            for (const vector<u8>& slot : slots)
            {
                if (!slot.empty())
                {
                    std::memcpy(blob.data() + offset, slot.data(), slot.size());
                    offset += slot.size();
                }
            }
            return blob;
        }

        // The schema-1 manifest: one `atlas` image and a declared `alpha` and `colorSpace`.
        Result<vector<u8>> CookVersionOne(const CookContext& context, const path& sourcePath,
                                          const json& manifest, const string& where)
        {
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
            if (manifest.contains("frameScale"))
            {
                return std::unexpected(fmt::format(
                    "flipbook importer: '{}': 'frameScale' needs schema version 2", where));
            }

            CookedFlipbookHeader header{};
            header.Version = CookedFlipbookVersion;
            const Result<Grid> grid = ReadGridAndTiming(manifest, header, where);
            if (!grid)
            {
                return std::unexpected(grid.error());
            }
            const Result<u32> atlasWidth = PositiveInt(atlas, "width", where);
            const Result<u32> atlasHeight = PositiveInt(atlas, "height", where);
            for (const Result<u32>* field : {&atlasWidth, &atlasHeight})
            {
                if (!*field)
                {
                    return std::unexpected(field->error());
                }
            }
            if (grid->Width != *atlasWidth || grid->Height != *atlasHeight)
            {
                return std::unexpected(fmt::format(
                    "flipbook importer: '{}': a {}x{} grid of {}x{} frames is not the {}x{} atlas",
                    where, header.Columns, header.Rows, header.FrameWidth, header.FrameHeight,
                    *atlasWidth, *atlasHeight));
            }

            if (const VoidResult blend = ReadBlend(manifest, header, where); !blend)
            {
                return std::unexpected(blend.error());
            }
            const Result<string> alpha = RequiredString(manifest, "alpha", where);
            const Result<string> colorSpace = RequiredString(manifest, "colorSpace", where);
            for (const Result<string>* field : {&alpha, &colorSpace})
            {
                if (!*field)
                {
                    return std::unexpected(field->error());
                }
            }

            // The texture cook's alpha key follows the sheet's: premultiplied colour filters as
            // stored, and an alpha the player ignores weights nothing.
            const char* textureAlpha = "data";
            if (*alpha == "premultiplied")
            {
                header.AlphaMode = 0;
                textureAlpha = "premultiplied";
            }
            else if (*alpha == "coverage")
            {
                header.AlphaMode = 1;
                textureAlpha = "straight";
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

            const Result<bool> srgb = ParseColorSpace(*colorSpace, "colorSpace", where);
            if (!srgb)
            {
                return std::unexpected(srgb.error());
            }
            if (const VoidResult placement = ReadPlacement(manifest, header, where); !placement)
            {
                return std::unexpected(placement.error());
            }

            // A linear sheet takes the Packed role rather than Mask, since every configuration maps
            // Packed to a full-channel codec and a sprite needs all four.
            std::array<vector<u8>, CookedFlipbookTextureSlots> slots;
            Result<vector<u8>> texture = CookSlot(context, sourcePath,
                                                  SlotCook{
                                                      .File = atlas["file"].get<string>(),
                                                      .Srgb = *srgb,
                                                      .Role = *srgb ? "Color" : "Packed",
                                                      .Alpha = textureAlpha,
                                                      .Label = "atlas",
                                                  },
                                                  *grid, where);
            if (!texture)
            {
                return std::unexpected(texture.error());
            }
            slots[PrimarySlot] = std::move(*texture);
            return AssembleBlob(header, {}, slots);
        }

        // One `textures` entry of a schema-2 manifest.
        struct TextureEntry
        {
            string File;
            bool Srgb = false;
            f32 Scale = 0.0f;
        };

        // Reads and checks one `textures` entry against the grid.
        Result<TextureEntry> ReadTextureEntry(const json& textures, const string& name,
                                              const Grid& grid, const string& where)
        {
            const string field = fmt::format("textures.{}", name);
            const json& entry = textures[name];
            if (!entry.is_object())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': '{}' must be an object", where, field));
            }
            if (!entry.contains("file") || !entry["file"].is_string() ||
                entry["file"].get<string>().empty())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': '{}' has no 'file'", where, field));
            }
            const string located = fmt::format("{}' in '{}", field, where);
            const Result<u32> width = PositiveInt(entry, "width", located);
            const Result<u32> height = PositiveInt(entry, "height", located);
            for (const Result<u32>* size : {&width, &height})
            {
                if (!*size)
                {
                    return std::unexpected(size->error());
                }
            }
            if (*width != grid.Width || *height != grid.Height)
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': '{}' is {}x{}, and the grid is {}x{}",
                                where, field, *width, *height, grid.Width, grid.Height));
            }
            const Result<string> colorSpace = RequiredString(entry, "colorSpace", located);
            if (!colorSpace)
            {
                return std::unexpected(colorSpace.error());
            }
            const Result<bool> srgb =
                ParseColorSpace(*colorSpace, fmt::format("{}.colorSpace", field), where);
            if (!srgb)
            {
                return std::unexpected(srgb.error());
            }
            TextureEntry read{.File = entry["file"].get<string>(), .Srgb = *srgb};
            if (entry.contains("scale"))
            {
                if (!entry["scale"].is_number() || !(entry["scale"].get<f64>() > 0.0) ||
                    !std::isfinite(entry["scale"].get<f64>()))
                {
                    return std::unexpected(
                        fmt::format("flipbook importer: '{}': '{}.scale' must be a positive number",
                                    where, field));
                }
                read.Scale = entry["scale"].get<f32>();
            }
            return read;
        }

        // Reads the optional `emission` ramp and `ignition` into the header.
        VoidResult ReadEmission(const json& manifest, CookedFlipbookHeader& header,
                                const string& where)
        {
            const json& emission = manifest["emission"];
            if (!emission.is_object())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': 'emission' must be an object", where));
            }
            if (emission.contains("ignition"))
            {
                const json& ignition = emission["ignition"];
                if (!ignition.is_number() || !(ignition.get<f64>() >= 0.0) ||
                    !(ignition.get<f64>() <= 1.0))
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': 'emission.ignition' must be in [0, 1]", where));
                }
                header.Ignition = ignition.get<f32>();
            }
            if (!emission.contains("ramp") || !emission["ramp"].is_array() ||
                emission["ramp"].empty() || emission["ramp"].size() > CookedFlipbookMaxEmissionKeys)
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': 'emission.ramp' must hold 1 to {} keys",
                                where, CookedFlipbookMaxEmissionKeys));
            }
            const json& ramp = emission["ramp"];
            for (usize index = 0; index < ramp.size(); ++index)
            {
                const json& key = ramp[index];
                const string field = fmt::format("emission.ramp[{}]", index);
                if (!key.is_object() || !key.contains("t") || !key["t"].is_number() ||
                    !(key["t"].get<f64>() >= 0.0) || !(key["t"].get<f64>() <= 1.0))
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': '{}.t' must be in [0, 1]", where, field));
                }
                const f32 t = key["t"].get<f32>();
                if (index > 0 && !(t > header.EmissionKeys[index - 1][0]))
                {
                    return std::unexpected(
                        fmt::format("flipbook importer: '{}': '{}.t' must exceed the key before it",
                                    where, field));
                }
                if (!key.contains("color") || !key["color"].is_array() || key["color"].size() != 3)
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': '{}.color' must be an [r, g, b] number array",
                        where, field));
                }
                header.EmissionKeys[index][0] = t;
                for (usize channel = 0; channel < 3; ++channel)
                {
                    const json& value = key["color"][channel];
                    if (!value.is_number() || !std::isfinite(value.get<f64>()) ||
                        value.get<f64>() < 0.0)
                    {
                        return std::unexpected(fmt::format(
                            "flipbook importer: '{}': '{}.color' must be finite and non-negative",
                            where, field));
                    }
                    header.EmissionKeys[index][channel + 1] = value.get<f32>();
                }
            }
            header.EmissionKeyCount = static_cast<u32>(ramp.size());
            return {};
        }

        // Reads the optional per-frame `frameScale`.
        Result<vector<f32>> ReadFrameScale(const json& manifest, u32 frames, const string& where)
        {
            vector<f32> scales;
            if (!manifest.contains("frameScale"))
            {
                return scales;
            }
            const json& value = manifest["frameScale"];
            if (!value.is_array() || value.size() != frames)
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': 'frameScale' must hold one entry per "
                                "frame ({})",
                                where, frames));
            }
            for (const json& entry : value)
            {
                if (!entry.is_number() || !(entry.get<f64>() > 0.0) || !(entry.get<f64>() <= 1.0))
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': every 'frameScale' entry must be in (0, 1]",
                        where));
                }
                scales.push_back(entry.get<f32>());
            }
            return scales;
        }

        // The schema-2 manifest: a `shading` mode and the named `textures` sharing its grid.
        Result<vector<u8>> CookVersionTwo(const CookContext& context, const path& sourcePath,
                                          const json& manifest, const string& where)
        {
            // Each schema-1 key below has a fixed meaning per texture in schema 2.
            for (const char* key : {"alpha", "atlas", "colorSpace"})
            {
                if (manifest.contains(key))
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': '{}' is a schema-1 field; schema 2 fixes each "
                        "texture's alpha and declares its sheets under 'textures'",
                        where, key));
                }
            }

            CookedFlipbookHeader header{};
            header.Version = CookedFlipbookVersion;
            const Result<Grid> grid = ReadGridAndTiming(manifest, header, where);
            if (!grid)
            {
                return std::unexpected(grid.error());
            }
            if (const VoidResult blend = ReadBlend(manifest, header, where); !blend)
            {
                return std::unexpected(blend.error());
            }
            if (const VoidResult placement = ReadPlacement(manifest, header, where); !placement)
            {
                return std::unexpected(placement.error());
            }
            header.AlphaMode = 0;

            const Result<string> shading = RequiredString(manifest, "shading", where);
            if (!shading)
            {
                return std::unexpected(shading.error());
            }
            const bool sixWay = *shading == "sixway";
            if (!sixWay && *shading != "unlit")
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': unknown shading '{}' (expected "
                                "\"unlit\" or \"sixway\")",
                                where, *shading));
            }
            header.Shading = sixWay ? 1u : 0u;

            if (!manifest.contains("textures") || !manifest["textures"].is_object())
            {
                return std::unexpected(
                    fmt::format("flipbook importer: '{}': missing 'textures' object", where));
            }
            const json& textures = manifest["textures"];
            const vector<string> required =
                sixWay ? vector<string>{"lightA", "lightB"} : vector<string>{"color"};
            for (const auto& item : textures.items())
            {
                const string& name = item.key();
                if (std::ranges::find(required, name) == required.end() && name != "motion")
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': 'textures.{}' is not a {} texture (expected {})",
                        where, name, *shading,
                        sixWay ? "\"lightA\", \"lightB\" and an optional \"motion\""
                               : "\"color\" and an optional \"motion\""));
                }
            }
            for (const string& name : required)
            {
                if (!textures.contains(name))
                {
                    return std::unexpected(
                        fmt::format("flipbook importer: '{}': a {} flipbook needs 'textures.{}'",
                                    where, *shading, name));
                }
            }

            std::array<optional<TextureEntry>, CookedFlipbookTextureSlots> entries;
            const std::array<const char*, 3> slotNames = {sixWay ? "lightA" : "color", "lightB",
                                                          "motion"};
            for (u32 slot = 0; slot < slotNames.size(); ++slot)
            {
                if (!textures.contains(slotNames[slot]))
                {
                    continue;
                }
                Result<TextureEntry> entry =
                    ReadTextureEntry(textures, slotNames[slot], *grid, where);
                if (!entry)
                {
                    return std::unexpected(entry.error());
                }
                entries[slot] = std::move(*entry);
            }

            if (entries[MotionSlot])
            {
                if (!(entries[MotionSlot]->Scale > 0.0f))
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': 'textures.motion' needs a positive 'scale'",
                        where));
                }
                if (entries[MotionSlot]->Srgb)
                {
                    return std::unexpected(fmt::format(
                        "flipbook importer: '{}': 'textures.motion.colorSpace' must be \"linear\"",
                        where));
                }
                header.MotionScale = entries[MotionSlot]->Scale;
            }
            header.FrameBlend = entries[MotionSlot] ? 2u : 1u;

            if (!sixWay)
            {
                for (const char* key : {"emission", "temperatureEncoding"})
                {
                    if (manifest.contains(key))
                    {
                        return std::unexpected(fmt::format(
                            "flipbook importer: '{}': '{}' applies to a sixway flipbook", where,
                            key));
                    }
                }
            }
            else
            {
                header.TemperatureEncoding = 1;
                if (manifest.contains("temperatureEncoding"))
                {
                    const Result<string> encoding =
                        RequiredString(manifest, "temperatureEncoding", where);
                    if (!encoding)
                    {
                        return std::unexpected(encoding.error());
                    }
                    if (*encoding == "linear")
                    {
                        header.TemperatureEncoding = 0;
                    }
                    else if (*encoding != "sqrt")
                    {
                        return std::unexpected(
                            fmt::format("flipbook importer: '{}': unknown temperatureEncoding "
                                        "'{}' (expected \"sqrt\" or \"linear\")",
                                        where, *encoding));
                    }
                }
                if (manifest.contains("emission"))
                {
                    if (const VoidResult emission = ReadEmission(manifest, header, where);
                        !emission)
                    {
                        return std::unexpected(emission.error());
                    }
                }
            }

            const Result<vector<f32>> frameScale =
                ReadFrameScale(manifest, header.FrameCount, where);
            if (!frameScale)
            {
                return std::unexpected(frameScale.error());
            }

            // Every texture's alpha has one meaning: coverage on the primary sheet (colour and
            // lightmaps both premultiplied by it), temperature on lightB, nothing on motion.
            std::array<optional<SlotCook>, CookedFlipbookTextureSlots> cooks;
            const TextureEntry& primary = *entries[PrimarySlot];
            cooks[PrimarySlot] = SlotCook{
                .File = primary.File,
                .Srgb = primary.Srgb,
                .Role = primary.Srgb ? "Color" : "Packed",
                .Alpha = "premultiplied",
                .Label = fmt::format("textures.{}", slotNames[PrimarySlot]),
            };
            if (sixWay)
            {
                const TextureEntry& lightB = *entries[LightBSlot];
                cooks[LightBSlot] = SlotCook{
                    .File = lightB.File,
                    .Srgb = lightB.Srgb,
                    .Role = lightB.Srgb ? "Color" : "Packed",
                    .Alpha = SplitTemperature ? "discard" : "data",
                    .Label = "textures.lightB",
                };
                if (SplitTemperature)
                {
                    cooks[TemperatureSlot] = SlotCook{
                        .File = lightB.File,
                        .Srgb = false,
                        .Role = "Mask",
                        .Alpha = "data",
                        .SourceChannel = "a",
                        .Label = "textures.lightB temperature",
                    };
                }
            }
            if (entries[MotionSlot])
            {
                cooks[MotionSlot] = SlotCook{
                    .File = entries[MotionSlot]->File,
                    .Srgb = false,
                    .Role = "Vector",
                    .Alpha = "data",
                    .Label = "textures.motion",
                };
            }

            std::array<vector<u8>, CookedFlipbookTextureSlots> slots;
            for (u32 slot = 0; slot < CookedFlipbookTextureSlots; ++slot)
            {
                if (!cooks[slot])
                {
                    continue;
                }
                Result<vector<u8>> texture =
                    CookSlot(context, sourcePath, *cooks[slot], *grid, where);
                if (!texture)
                {
                    return std::unexpected(texture.error());
                }
                slots[slot] = std::move(*texture);
            }
            return AssembleBlob(header, *frameScale, slots);
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

        const i64 version = manifest.contains("version") && manifest["version"].is_number_integer()
                                ? manifest["version"].get<i64>()
                                : 0;
        if (version != SchemaVersionOne && version != SchemaVersionTwo)
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': unsupported atlas schema 'version' (expected "
                            "{} or {})",
                            where, SchemaVersionOne, SchemaVersionTwo));
        }
        if (!manifest.contains("type") || !manifest["type"].is_string() ||
            manifest["type"].get<string>() != SchemaType)
        {
            return std::unexpected(
                fmt::format("flipbook importer: '{}': 'type' must be \"{}\"", where, SchemaType));
        }

        return version == SchemaVersionOne ? CookVersionOne(context, sourcePath, manifest, where)
                                           : CookVersionTwo(context, sourcePath, manifest, where);
    }
}
