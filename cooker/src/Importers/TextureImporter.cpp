#include "TextureImporter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <astcenc.h>
#include <bc7enc.h>
#include <fmt/format.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>

namespace Veng::Cook
{
    namespace
    {
        // The texture cook's compression codec, the encode-path selector. ASTC is the
        // Metal-blessed codec on the primary MoltenVK platform; BC7 targets the desktop/Windows
        // platform; None packs uncompressed RGBA8. The raw "compression": "astc"/"bc7"/"none"
        // escape-hatch key pins one directly; otherwise the codec is derived from the resolved
        // CompressionFormat (role table or the hardcoded ASTC zero-config default).
        enum class TextureCodec
        {
            None,
            BC7,
            ASTC,
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
            if (name == "astc")
            {
                return TextureCodec::ASTC;
            }
            return std::nullopt;
        }

        // Renderer::Format ordinals (Types.h), hand-synced per the cycle-avoidance rule:
        // 2 = RGBA8Unorm, 3 = RGBA8Srgb, 21 = BC7Unorm, 22 = BC7Srgb, 23 = ASTC4x4Unorm,
        // 24 = ASTC4x4Srgb. CookedTextureHeader.Format stores these literals and the engine's
        // TextureLoader::BridgeFormat reads them back.
        constexpr u32 RGBA8UnormFormat = 2;
        constexpr u32 RGBA8SrgbFormat = 3;
        constexpr u32 BC7UnormFormat = 21;
        constexpr u32 BC7SrgbFormat = 22;
        constexpr u32 ASTC4x4UnormFormat = 23;
        constexpr u32 ASTC4x4SrgbFormat = 24;

        // A texture's cook is one codec + one header Format ordinal. The codec drives the encode
        // path (block-compress vs. raw copy); the ordinal is the hand-synced Renderer::Format value
        // the engine's TextureLoader::BridgeFormat reads back.
        struct ResolvedFormat
        {
            TextureCodec Codec{};
            u32 FormatOrdinal{};
        };

        // Parses a "role" name to its CompressionRole. The names match the canonical authoring
        // spellings in Veng::ToString(CompressionRole); the mapping is duplicated here rather than
        // calling the libveng symbol, since the texture importer links into the veng-free cooker
        // core (and the core-pack bootstrap) that cannot reference libveng's out-of-line tables.
        optional<CompressionRole> ParseRole(const string& name)
        {
            if (name == "Color")
            {
                return CompressionRole::Color;
            }
            if (name == "Normal")
            {
                return CompressionRole::Normal;
            }
            if (name == "Mask")
            {
                return CompressionRole::Mask;
            }
            if (name == "HDR")
            {
                return CompressionRole::HDR;
            }
            if (name == "UI")
            {
                return CompressionRole::UI;
            }
            return std::nullopt;
        }

        // Reads the configuration's resolved format for a role from its fixed RoleToFormat record.
        CompressionFormat RoleFormat(const RoleToFormat& table, CompressionRole role)
        {
            switch (role)
            {
            case CompressionRole::Color:
                return table.Color;
            case CompressionRole::Normal:
                return table.Normal;
            case CompressionRole::Mask:
                return table.Mask;
            case CompressionRole::HDR:
                return table.HDR;
            case CompressionRole::UI:
                return table.UI;
            }
            return table.Color;
        }

        // Lowers a CompressionFormat (the closed codec-output set a role table holds) to the cook's
        // codec + header Format ordinal. RGBA16Sfloat has no LDR encode path in this importer, so a
        // role resolving to it is an error here — an HDR source is an environment asset.
        Result<ResolvedFormat> ResolveCompressionFormat(CompressionFormat format)
        {
            switch (format)
            {
            case CompressionFormat::RGBA8Unorm:
                return ResolvedFormat{.Codec = TextureCodec::None,
                                      .FormatOrdinal = RGBA8UnormFormat};
            case CompressionFormat::RGBA8Srgb:
                return ResolvedFormat{.Codec = TextureCodec::None,
                                      .FormatOrdinal = RGBA8SrgbFormat};
            case CompressionFormat::BC7Unorm:
                return ResolvedFormat{.Codec = TextureCodec::BC7, .FormatOrdinal = BC7UnormFormat};
            case CompressionFormat::BC7Srgb:
                return ResolvedFormat{.Codec = TextureCodec::BC7, .FormatOrdinal = BC7SrgbFormat};
            case CompressionFormat::ASTC4x4Unorm:
                return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                      .FormatOrdinal = ASTC4x4UnormFormat};
            case CompressionFormat::ASTC4x4Srgb:
                return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                      .FormatOrdinal = ASTC4x4SrgbFormat};
            case CompressionFormat::RGBA16Sfloat:
                return std::unexpected(
                    string("the HDR role resolves to RGBA16Sfloat, which an LDR texture cannot "
                           "encode; author an HDR source as an environment asset"));
            }
            return std::unexpected(string("unmapped CompressionFormat"));
        }

        // The codec + ordinal a raw "compression" codec name pins, keyed off the texture's sRGB
        // flag for its sRGB-aware format pair. This is the escape-hatch path: it wins over the role.
        ResolvedFormat RawCodecFormat(TextureCodec codec, bool srgb)
        {
            switch (codec)
            {
            case TextureCodec::ASTC:
                return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                      .FormatOrdinal =
                                          srgb ? ASTC4x4SrgbFormat : ASTC4x4UnormFormat};
            case TextureCodec::BC7:
                return ResolvedFormat{.Codec = TextureCodec::BC7,
                                      .FormatOrdinal = srgb ? BC7SrgbFormat : BC7UnormFormat};
            case TextureCodec::None:
                return ResolvedFormat{.Codec = TextureCodec::None,
                                      .FormatOrdinal = srgb ? RGBA8SrgbFormat : RGBA8UnormFormat};
            }
            return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                  .FormatOrdinal = srgb ? ASTC4x4SrgbFormat : ASTC4x4UnormFormat};
        }

        // BC7 and ASTC 4x4 both encode a 4x4 texel tile into one 16-byte block; the full mip chain
        // (down to 1x1) pads partial edge tiles by replicating the level's edge texels into the 4x4
        // block.
        constexpr u32 BlockSize = 4;
        constexpr usize BlockBytes = 16;

        // BC7 quality preset. Golden stability depends on this preset staying fixed: a higher
        // uber level or partition count would re-encode every block and move any golden capture.
        // Defaults (perceptual weights, all modes, 64 partitions) at uber level 1 — a balanced
        // quality/speed point above the level-0 default.
        constexpr u32 BC7UberLevel = 1;

        // ASTC 4x4 quality preset, expressed as an astcenc effort level in [0, 100]. Golden and
        // cook determinism depend on this staying fixed: ASTCENC_PRE_MEDIUM (60) paired with the
        // ISA "none" scalar codec and ASTCENC_INVARIANCE produces reproducible blocks run to run, a
        // balanced quality/speed point. (ASTCENC_PRE_MEDIUM is a static const float, not constexpr.)
        const f32 AstcQuality = ASTCENC_PRE_MEDIUM;

        // Encodes one RGBA8 mip level (tightly packed, row-major) to ASTC 4x4 LDR blocks through the
        // ARM astc-encoder. The encoder pads partial edge tiles internally, so the full chain down
        // to 1x1 encodes. @p profile selects the sRGB-aware LDR profile.
        //
        // The level's blocks are encoded across hardware_concurrency() threads — the encoder's
        // documented model: one context allocated for N threads, then N calls to
        // astcenc_compress_image, each from a distinct thread under its own [0..N-1] index, with the
        // blocks dynamically scheduled across them. ASTCENC_INVARIANCE makes the encode independent
        // of thread count, so the cooked blocks are byte-identical at any N and the smoke golden does
        // not move.
        Result<vector<u8>> EncodeAstcLevel(u8* rgba, u32 width, u32 height, astcenc_profile profile)
        {
            astcenc_config config{};
            const astcenc_error configStatus =
                astcenc_config_init(profile, BlockSize, BlockSize, 1, AstcQuality,
                                    ASTCENC_FLG_USE_DECODE_UNORM8, &config);
            if (configStatus != ASTCENC_SUCCESS)
            {
                return std::unexpected(fmt::format("texture importer: ASTC config init failed: {}",
                                                   astcenc_get_error_string(configStatus)));
            }

            const u32 blocksWide = (width + BlockSize - 1) / BlockSize;
            const u32 blocksHigh = (height + BlockSize - 1) / BlockSize;
            const u32 blockCount = blocksWide * blocksHigh;

            // Cap the worker count at the level's block count so a tiny mip does not spawn idle
            // threads (the encoder schedules whole blocks, never a fraction of one).
            const u32 hardware = std::max(1u, std::thread::hardware_concurrency());
            const u32 threadCount = std::min(hardware, std::max(1u, blockCount));

            astcenc_context* context = nullptr;
            const astcenc_error allocStatus = astcenc_context_alloc(&config, threadCount, &context);
            if (allocStatus != ASTCENC_SUCCESS)
            {
                return std::unexpected(
                    fmt::format("texture importer: ASTC context alloc failed: {}",
                                astcenc_get_error_string(allocStatus)));
            }

            void* slice = rgba; // NOLINT(misc-const-correctness)
            astcenc_image image{
                .dim_x = width,
                .dim_y = height,
                .dim_z = 1,
                .data_type = ASTCENC_TYPE_U8,
                .data = &slice,
            };

            const astcenc_swizzle swizzle{
                .r = ASTCENC_SWZ_R,
                .g = ASTCENC_SWZ_G,
                .b = ASTCENC_SWZ_B,
                .a = ASTCENC_SWZ_A,
            };

            vector<u8> blocks(static_cast<usize>(blocksWide) * blocksHigh * BlockBytes);

            // Each thread compresses its share of the blocks under a unique index; thread 0 runs on
            // the calling thread while the rest run on spawned workers, all joined before the context
            // is freed.
            vector<astcenc_error> statuses(threadCount, ASTCENC_SUCCESS);
            const auto compressShare = [&](u32 threadIndex)
            {
                statuses[threadIndex] = astcenc_compress_image(
                    context, &image, &swizzle, blocks.data(), blocks.size(), threadIndex);
            };

            vector<std::thread> workers;
            workers.reserve(threadCount - 1);
            for (u32 i = 1; i < threadCount; i++)
            {
                workers.emplace_back(compressShare, i);
            }
            compressShare(0);
            for (std::thread& worker : workers)
            {
                worker.join();
            }
            astcenc_context_free(context);

            for (const astcenc_error status : statuses)
            {
                if (status != ASTCENC_SUCCESS)
                {
                    return std::unexpected(fmt::format("texture importer: ASTC compress failed: {}",
                                                       astcenc_get_error_string(status)));
                }
            }

            return blocks;
        }

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

        // The format-resolution chain: the raw "compression" codec name (the escape hatch) wins,
        // else the build configuration's role table, else the hardcoded ASTC zero-config default.
        // A texture declares a "role" (its intent — Color/Normal/Mask/HDR/UI), parsed to a
        // CompressionRole that the active configuration maps to a concrete codec output. Absent a
        // role, the intent is guessed from the sRGB flag: an sRGB source is Color, a non-sRGB source
        // is Mask (the safe unorm default); Normal/HDR/UI cannot be told apart by the flag and are
        // authored explicitly.
        const optional<TextureCodec> rawCodec = [&]() -> optional<TextureCodec>
        {
            if (texJson.contains("compression") && texJson["compression"].is_string())
            {
                return ParseCodec(texJson["compression"].get<string>());
            }
            return std::nullopt;
        }();
        if (texJson.contains("compression") && texJson["compression"].is_string() && !rawCodec)
        {
            return std::unexpected(
                fmt::format("texture importer: '{}': invalid compression '{}' (expected "
                            "'astc', 'bc7', or 'none')",
                            sourcePath.string(), texJson["compression"].get<string>()));
        }

        CompressionRole role = srgb ? CompressionRole::Color : CompressionRole::Mask;
        if (texJson.contains("role") && texJson["role"].is_string())
        {
            const string roleName = texJson["role"].get<string>();
            const optional<CompressionRole> parsed = ParseRole(roleName);
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': invalid role '{}' (expected "
                                "'Color', 'Normal', 'Mask', 'HDR', or 'UI')",
                                sourcePath.string(), roleName));
            }
            role = *parsed;
        }

        ResolvedFormat resolved{};
        if (rawCodec)
        {
            resolved = RawCodecFormat(*rawCodec, srgb);
        }
        else if (context.Config != nullptr)
        {
            const CompressionFormat roleFormat = RoleFormat(context.Config->Formats, role);
            const Result<ResolvedFormat> lowered = ResolveCompressionFormat(roleFormat);
            if (!lowered)
            {
                return std::unexpected(fmt::format("texture importer: '{}': {}",
                                                   sourcePath.string(), lowered.error()));
            }
            resolved = *lowered;
        }
        else
        {
            // Zero-config default: the hardcoded ASTC codec, the Metal-blessed codec on the primary
            // platform, preserved bit-for-bit for an un-migrated project.
            resolved = RawCodecFormat(TextureCodec::ASTC, srgb);
        }

        const TextureCodec codec = resolved.Codec;

        // The resolved format is authoritative for sRGB-ness: it carries the role's intent, so a
        // config-driven Color → *Srgb encodes (and resizes) in gamma space regardless of the source
        // "srgb" flag. The override and zero-config ordinals are keyed off "srgb", so this equals the
        // flag on those paths — the encode stays byte-identical.
        const bool srgbEncode = resolved.FormatOrdinal == RGBA8SrgbFormat ||
                                resolved.FormatOrdinal == BC7SrgbFormat ||
                                resolved.FormatOrdinal == ASTC4x4SrgbFormat;

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
                srgbEncode ? stbir_resize_uint8_srgb(pixels, width, height, 0, pixelData.data(),
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

        CookedTextureHeader header{};
        header.Format = resolved.FormatOrdinal;
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

        // The byte size of one mip level in the chosen codec: 4x4 16-byte blocks
        // (ceil(w/4)*ceil(h/4)*16) for BC7 and ASTC, or uncompressed RGBA8 (w*h*4). Mirrors the
        // engine's BytesForLevel.
        const bool blockCodec = codec == TextureCodec::BC7 || codec == TextureCodec::ASTC;
        const auto levelBytes = [blockCodec](u32 levelWidth, u32 levelHeight) -> usize
        {
            if (blockCodec)
            {
                const u32 blocksWide = (levelWidth + BlockSize - 1) / BlockSize;
                const u32 blocksHigh = (levelHeight + BlockSize - 1) / BlockSize;
                return static_cast<usize>(blocksWide) * blocksHigh * BlockBytes;
            }
            return static_cast<usize>(levelWidth) * levelHeight * 4;
        };

        // Encodes one RGBA8 level into the codec's on-disk bytes at blob[writeOffset]. BC7 and ASTC
        // pack 4x4 blocks; None copies the RGBA8 bytes through unchanged. The ASTC profile is the
        // sRGB-aware LDR profile matching the chosen format pair.
        bc7enc_compress_block_params params{};
        if (codec == TextureCodec::BC7)
        {
            bc7enc_compress_block_init();
            bc7enc_compress_block_params_init(&params);
            params.m_uber_level = BC7UberLevel;
        }
        const astcenc_profile astcProfile = srgbEncode ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;

        const auto packLevel = [&](vector<u8>& blob, usize writeOffset, u8* rgba, u32 lw,
                                   u32 lh) -> VoidResult
        {
            if (codec == TextureCodec::BC7)
            {
                const vector<u8> blocks = EncodeBc7Level(rgba, lw, lh, params);
                std::memcpy(blob.data() + writeOffset, blocks.data(), blocks.size());
            }
            else if (codec == TextureCodec::ASTC)
            {
                const Result<vector<u8>> blocks = EncodeAstcLevel(rgba, lw, lh, astcProfile);
                if (!blocks)
                {
                    return std::unexpected(blocks.error());
                }
                std::memcpy(blob.data() + writeOffset, blocks->data(), blocks->size());
            }
            else
            {
                std::memcpy(blob.data() + writeOffset, rgba, static_cast<usize>(lw) * lh * 4);
            }
            return {};
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
        if (const VoidResult packed =
                packLevel(blob, writeOffset, pixelData.data(), baseWidth, baseHeight);
            !packed)
        {
            return std::unexpected(packed.error());
        }
        writeOffset += levelBytes(baseWidth, baseHeight);

        for (u32 level = 1; level < mipCount; level++)
        {
            const u32 levelWidth = std::max(1u, baseWidth >> level);
            const u32 levelHeight = std::max(1u, baseHeight >> level);

            vector<u8> levelRgba(static_cast<usize>(levelWidth) * levelHeight * 4);

            const stbir_pixel_layout layout = STBIR_RGBA;
            const unsigned char* resized =
                srgbEncode
                    ? stbir_resize_uint8_srgb(pixelData.data(), targetWidth, targetHeight, 0,
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

            if (const VoidResult packed =
                    packLevel(blob, writeOffset, levelRgba.data(), levelWidth, levelHeight);
                !packed)
            {
                return std::unexpected(packed.error());
            }
            writeOffset += levelBytes(levelWidth, levelHeight);
        }

        return blob;
    }
}
