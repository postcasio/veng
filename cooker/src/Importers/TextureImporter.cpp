#include "TextureImporter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>

#include <bc7enc.h>
#include <fmt/format.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng::Cook
{
    namespace
    {
        // The texture cook's compression codec. None packs uncompressed RGBA8; BC7 is the only
        // block codec so far. This is the minimal internal seam where a per-texture/per-pack codec
        // choice attaches — a texture opts into BC7 with "compression": "bc7", and absent the key
        // a texture stays uncompressed RGBA8.
        enum class TextureCodec
        {
            None,
            BC7,
        };

        optional<TextureCodec> ParseCodec(const string& name)
        {
            if (name == "none")
            {
                return TextureCodec::None;
            }
            if (name == "bc7")
            {
                return TextureCodec::BC7;
            }
            return std::nullopt;
        }

        // Renderer::Format ordinals (Types.h), hand-synced per the cycle-avoidance rule:
        // 2 = RGBA8Unorm, 3 = RGBA8Srgb, 21 = BC7Unorm, 22 = BC7Srgb. CookedTextureHeader.Format
        // stores these literals and the engine's TextureLoader::BridgeFormat reads them back.
        constexpr u32 RGBA8UnormFormat = 2;
        constexpr u32 RGBA8SrgbFormat = 3;
        constexpr u32 BC7UnormFormat = 21;
        constexpr u32 BC7SrgbFormat = 22;

        // BC7 encodes a 4x4 texel tile into one 16-byte block; the full mip chain (down to 1x1)
        // pads partial edge tiles by replicating the level's edge texels into the 4x4 block.
        constexpr u32 BlockSize = 4;
        constexpr usize BlockBytes = 16;

        // BC7 quality preset. Golden stability depends on this preset staying fixed: a higher
        // uber level or partition count would re-encode every block and move any golden capture.
        // Defaults (perceptual weights, all modes, 64 partitions) at uber level 1 — a balanced
        // quality/speed point above the level-0 default.
        constexpr u32 BC7UberLevel = 1;

        // Encodes one RGBA8 mip level (tightly packed, row-major) to BC7 blocks. Partial edge
        // tiles on a non-multiple-of-4 level replicate the nearest in-bounds texel into the 4x4
        // source block, the standard edge-padding for block compression.
        vector<u8> EncodeBc7Level(const u8* rgba, u32 width, u32 height,
                                  const bc7enc_compress_block_params& params)
        {
            const u32 blocksWide = (width + BlockSize - 1) / BlockSize;
            const u32 blocksHigh = (height + BlockSize - 1) / BlockSize;

            vector<u8> blocks(static_cast<usize>(blocksWide) * blocksHigh * BlockBytes);

            for (u32 by = 0; by < blocksHigh; by++)
            {
                for (u32 bx = 0; bx < blocksWide; bx++)
                {
                    std::array<u8, BlockSize * BlockSize * 4> source{};
                    for (u32 py = 0; py < BlockSize; py++)
                    {
                        const u32 sy = std::min(by * BlockSize + py, height - 1);
                        for (u32 px = 0; px < BlockSize; px++)
                        {
                            const u32 sx = std::min(bx * BlockSize + px, width - 1);
                            const usize src = (static_cast<usize>(sy) * width + sx) * 4;
                            const usize dst = (static_cast<usize>(py) * BlockSize + px) * 4;
                            source[dst + 0] = rgba[src + 0];
                            source[dst + 1] = rgba[src + 1];
                            source[dst + 2] = rgba[src + 2];
                            source[dst + 3] = rgba[src + 3];
                        }
                    }

                    const usize blockIndex = static_cast<usize>(by) * blocksWide + bx;
                    bc7enc_compress_block(blocks.data() + blockIndex * BlockBytes, source.data(),
                                          &params);
                }
            }

            return blocks;
        }

        // The Parse* helpers below mirror Veng::Renderer::Filter / MipmapMode /
        // AddressMode ordinals (Renderer/Types.h) — kept in sync by hand per the
        // cycle-avoidance rule documented in assetpack's CookedBlobs.h.

        optional<u32> ParseFilter(const string& name)
        {
            if (name == "nearest")
            {
                return 0u;
            }
            if (name == "linear")
            {
                return 1u;
            }
            return std::nullopt;
        }

        optional<u32> ParseMipmapMode(const string& name)
        {
            if (name == "nearest")
            {
                return 0u;
            }
            if (name == "linear")
            {
                return 1u;
            }
            return std::nullopt;
        }

        optional<u32> ParseAddressMode(const string& name)
        {
            if (name == "repeat")
            {
                return 0u;
            }
            if (name == "mirrored_repeat")
            {
                return 1u;
            }
            if (name == "clamp_to_edge")
            {
                return 2u;
            }
            if (name == "clamp_to_border")
            {
                return 3u;
            }
            return std::nullopt;
        }
    }

    Result<vector<u8>> TextureImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("texture importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
        {
            return std::unexpected(
                fmt::format("texture importer: failed to open '{}'", sourcePath.string()));
        }

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json texJson = json::parse(contentStream.str(), nullptr, false);
        if (texJson.is_discarded() || !texJson.is_object())
        {
            return std::unexpected(
                fmt::format("texture importer: '{}': invalid JSON", sourcePath.string()));
        }

        if (!texJson.contains("image") || !texJson["image"].is_string())
        {
            return std::unexpected(fmt::format("texture importer: '{}': missing or invalid 'image'",
                                               sourcePath.string()));
        }

        // Mips are generated by default; "generate_mips": false opts back out to a single level.
        const bool generateMips =
            !(texJson.contains("generate_mips") && texJson["generate_mips"].is_boolean() &&
              !texJson["generate_mips"].get<bool>());

        const bool srgb =
            texJson.contains("srgb") && texJson["srgb"].is_boolean() && texJson["srgb"].get<bool>();

        // The codec-selection seam: "compression" picks the block codec (BC7 only so far);
        // absent or "none" cooks uncompressed RGBA8. sRGB-aware — the format pair is chosen from
        // the texture's sRGB flag below.
        TextureCodec codec = TextureCodec::None;
        if (texJson.contains("compression") && texJson["compression"].is_string())
        {
            const string codecName = texJson["compression"].get<string>();
            const optional<TextureCodec> parsed = ParseCodec(codecName);
            if (!parsed)
            {
                return std::unexpected(fmt::format(
                    "texture importer: '{}': invalid compression '{}' (expected 'none' or 'bc7')",
                    sourcePath.string(), codecName));
            }
            codec = *parsed;
        }

        const path imagePath = sourcePath.parent_path() / texJson["image"].get<string>();
        context.RecordDependency(imagePath);

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(imagePath.string().c_str(), &width, &height, &channels, 4);
        if (!pixels)
        {
            return std::unexpected(
                fmt::format("texture importer: '{}': failed to load image '{}': {}",
                            sourcePath.string(), imagePath.string(), stbi_failure_reason()));
        }

        // Optional downscale: when the larger edge exceeds "max_size", shrink the image
        // (aspect-preserving) before packing so high-resolution source art does not bloat
        // the raw-pixel cooked blob. sRGB textures resize in gamma space, linear in linear.
        u32 maxSize = 0;
        if (texJson.contains("max_size") && texJson["max_size"].is_number_unsigned())
        {
            maxSize = texJson["max_size"].get<u32>();
        }

        int targetWidth = width;
        int targetHeight = height;
        if (maxSize > 0 &&
            (static_cast<u32>(width) > maxSize || static_cast<u32>(height) > maxSize))
        {
            const f32 scale = static_cast<f32>(maxSize) / static_cast<f32>(std::max(width, height));
            targetWidth = std::max(1, static_cast<int>(static_cast<f32>(width) * scale));
            targetHeight = std::max(1, static_cast<int>(static_cast<f32>(height) * scale));
        }

        const usize pixelBytes =
            static_cast<usize>(targetWidth) * static_cast<usize>(targetHeight) * 4;
        vector<u8> pixelData(pixelBytes);

        if (targetWidth != width || targetHeight != height)
        {
            const stbir_pixel_layout layout = STBIR_RGBA;
            const unsigned char* resized =
                srgb ? stbir_resize_uint8_srgb(pixels, width, height, 0, pixelData.data(),
                                               targetWidth, targetHeight, 0, layout)
                     : stbir_resize_uint8_linear(pixels, width, height, 0, pixelData.data(),
                                                 targetWidth, targetHeight, 0, layout);
            if (resized == nullptr)
            {
                stbi_image_free(pixels);
                return std::unexpected(fmt::format("texture importer: '{}': failed to resize '{}'",
                                                   sourcePath.string(), imagePath.string()));
            }
        }
        else
        {
            std::memcpy(pixelData.data(), pixels, pixelBytes);
        }
        stbi_image_free(pixels);

        const u32 baseWidth = static_cast<u32>(targetWidth);
        const u32 baseHeight = static_cast<u32>(targetHeight);

        u32 mipCount = 1;
        if (generateMips)
        {
            u32 largestEdge = std::max(baseWidth, baseHeight);
            while (largestEdge > 1)
            {
                largestEdge >>= 1;
                mipCount++;
            }
        }

        // Format is the codec's sRGB-aware pair: BC7Srgb/Unorm for the block codec, RGBA8Srgb/Unorm
        // uncompressed, keyed off the texture's sRGB flag.
        u32 format = 0;
        switch (codec)
        {
        case TextureCodec::BC7:
            format = srgb ? BC7SrgbFormat : BC7UnormFormat;
            break;
        case TextureCodec::None:
            format = srgb ? RGBA8SrgbFormat : RGBA8UnormFormat;
            break;
        }

        CookedTextureHeader header{};
        header.Format = format;
        header.Width = baseWidth;
        header.Height = baseHeight;
        header.MipCount = mipCount;

        // Sampler defaults mirror Veng::Renderer::SamplerInfo's defaults
        // (Renderer/Sampler.h).
        header.MinFilter = 1;    // Linear
        header.MagFilter = 1;    // Linear
        header.MipmapMode = 1;   // Linear
        header.AddressModeU = 0; // Repeat
        header.AddressModeV = 0; // Repeat
        header.AddressModeW = 0; // Repeat
        header.AnisotropyEnabled = 1;
        header.MaxAnisotropy = 8.0f;

        if (texJson.contains("sampler") && texJson["sampler"].is_object())
        {
            const json& sampler = texJson["sampler"];

            // Reads a string field through `parser`, returning `fallback` if the
            // field is absent.
            auto field = [&](const char* key, auto parser, u32 fallback) -> Result<u32>
            {
                if (!sampler.contains(key) || !sampler[key].is_string())
                {
                    return fallback;
                }

                const string value = sampler[key].get<string>();
                const optional<u32> parsed = parser(value);
                if (!parsed)
                {
                    return std::unexpected(
                        fmt::format("texture importer: '{}': invalid sampler.{} '{}'",
                                    sourcePath.string(), key, value));
                }

                return *parsed;
            };

            const Result<u32> minFilter = field("min", ParseFilter, header.MinFilter);
            if (!minFilter)
            {
                return std::unexpected(minFilter.error());
            }
            header.MinFilter = *minFilter;

            const Result<u32> magFilter = field("mag", ParseFilter, header.MagFilter);
            if (!magFilter)
            {
                return std::unexpected(magFilter.error());
            }
            header.MagFilter = *magFilter;

            const Result<u32> mipmapMode = field("mipmap", ParseMipmapMode, header.MipmapMode);
            if (!mipmapMode)
            {
                return std::unexpected(mipmapMode.error());
            }
            header.MipmapMode = *mipmapMode;

            const Result<u32> addressModeU = field("wrap_u", ParseAddressMode, header.AddressModeU);
            if (!addressModeU)
            {
                return std::unexpected(addressModeU.error());
            }
            header.AddressModeU = *addressModeU;

            const Result<u32> addressModeV = field("wrap_v", ParseAddressMode, header.AddressModeV);
            if (!addressModeV)
            {
                return std::unexpected(addressModeV.error());
            }
            header.AddressModeV = *addressModeV;

            const Result<u32> addressModeW = field("wrap_w", ParseAddressMode, header.AddressModeW);
            if (!addressModeW)
            {
                return std::unexpected(addressModeW.error());
            }
            header.AddressModeW = *addressModeW;

            if (sampler.contains("anisotropy") && sampler["anisotropy"].is_number())
            {
                const f32 value = sampler["anisotropy"].get<f32>();
                header.AnisotropyEnabled = value > 0.0f ? 1u : 0u;
                header.MaxAnisotropy = value > 0.0f ? value : 1.0f;
            }
        }

        // The byte size of one mip level in the chosen codec: BC7 blocks (ceil(w/4)*ceil(h/4)*16)
        // or uncompressed RGBA8 (w*h*4). Mirrors the engine's BytesForLevel.
        const auto levelBytes = [codec](u32 levelWidth, u32 levelHeight) -> usize
        {
            if (codec == TextureCodec::BC7)
            {
                const u32 blocksWide = (levelWidth + BlockSize - 1) / BlockSize;
                const u32 blocksHigh = (levelHeight + BlockSize - 1) / BlockSize;
                return static_cast<usize>(blocksWide) * blocksHigh * BlockBytes;
            }
            return static_cast<usize>(levelWidth) * levelHeight * 4;
        };

        // Encodes one RGBA8 level into the codec's on-disk bytes at blob[writeOffset]. BC7 packs
        // 4x4 blocks; None copies the RGBA8 bytes through unchanged.
        bc7enc_compress_block_params params{};
        if (codec == TextureCodec::BC7)
        {
            bc7enc_compress_block_init();
            bc7enc_compress_block_params_init(&params);
            params.m_uber_level = BC7UberLevel;
        }

        const auto packLevel =
            [&](vector<u8>& blob, usize writeOffset, const u8* rgba, u32 lw, u32 lh)
        {
            if (codec == TextureCodec::BC7)
            {
                const vector<u8> blocks = EncodeBc7Level(rgba, lw, lh, params);
                std::memcpy(blob.data() + writeOffset, blocks.data(), blocks.size());
            }
            else
            {
                std::memcpy(blob.data() + writeOffset, rgba, static_cast<usize>(lw) * lh * 4);
            }
        };

        // Each mip's RGBA8 pixels are generated (level 0 is the decoded base; each successive
        // level is resized from the full-resolution base, sRGB-correct for an sRGB source and
        // linear otherwise, so rounding does not accumulate), then packed largest-first in the
        // chosen codec. Resizing from the base rather than the previous mip keeps quality stable.
        usize totalLevelBytes = 0;
        for (u32 level = 0; level < mipCount; level++)
        {
            const u32 levelWidth = std::max(1u, baseWidth >> level);
            const u32 levelHeight = std::max(1u, baseHeight >> level);
            totalLevelBytes += levelBytes(levelWidth, levelHeight);
        }

        vector<u8> blob(sizeof(CookedTextureHeader) + totalLevelBytes);
        std::memcpy(blob.data(), &header, sizeof(header));

        usize writeOffset = sizeof(header);

        // Level 0 packs from the decoded (possibly downscaled) base pixels directly.
        packLevel(blob, writeOffset, pixelData.data(), baseWidth, baseHeight);
        writeOffset += levelBytes(baseWidth, baseHeight);

        for (u32 level = 1; level < mipCount; level++)
        {
            const u32 levelWidth = std::max(1u, baseWidth >> level);
            const u32 levelHeight = std::max(1u, baseHeight >> level);

            vector<u8> levelRgba(static_cast<usize>(levelWidth) * levelHeight * 4);

            const stbir_pixel_layout layout = STBIR_RGBA;
            const unsigned char* resized =
                srgb ? stbir_resize_uint8_srgb(pixelData.data(), targetWidth, targetHeight, 0,
                                               levelRgba.data(), static_cast<int>(levelWidth),
                                               static_cast<int>(levelHeight), 0, layout)
                     : stbir_resize_uint8_linear(pixelData.data(), targetWidth, targetHeight, 0,
                                                 levelRgba.data(), static_cast<int>(levelWidth),
                                                 static_cast<int>(levelHeight), 0, layout);
            if (resized == nullptr)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': failed to generate mip level {}",
                                sourcePath.string(), level));
            }

            packLevel(blob, writeOffset, levelRgba.data(), levelWidth, levelHeight);
            writeOffset += levelBytes(levelWidth, levelHeight);
        }

        return blob;
    }
}
