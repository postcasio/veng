#include "TextureImporter.h"
#include <Veng/Asset/Path.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <thread>

#include <astcenc.h>
#include <bc7enc.h>
#include <fmt/format.h>
#include <rgbcx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <tinyexr.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Renderer/TypeNames.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Cook
{
    namespace
    {
        // The texture cook's compression codec, the encode-path selector. ASTC is the
        // Metal-blessed codec on the primary MoltenVK platform; BC7 targets the desktop/Windows
        // platform; BC5/BC4 are the channel-specialized block codecs for a Normal/Mask role on
        // the BC target; None packs uncompressed RGBA8; Half packs uncompressed half-float texels
        // from a float source. The raw "compression": "ASTC"/"BC7"/"None"/"RGBA16F" escape-hatch
        // key pins one directly; otherwise the codec is derived from the resolved
        // CompressionFormat (role table or the hardcoded ASTC zero-config default). BC5/BC4 have no
        // raw escape-hatch spelling — they ride the role table only, since the role carries the
        // channel intent the raw codec name lacks.
        enum class TextureCodec
        {
            None,
            BC7,
            BC5,
            BC4,
            ASTC,
            Half,
        };

        optional<TextureCodec> ParseCodec(const string& name)
        {
            if (name == "None")
            {
                return TextureCodec::None;
            }
            if (name == "BC7")
            {
                return TextureCodec::BC7;
            }
            if (name == "ASTC")
            {
                return TextureCodec::ASTC;
            }
            if (name == "RGBA16F")
            {
                return TextureCodec::Half;
            }
            return std::nullopt;
        }

        // A texture's cook is one codec + one header format + a channel layout. The codec drives
        // the encode path (block-compress vs. raw copy); the format is the Renderer::Format whose
        // ordinal CookedTextureHeader.Format stores and the engine's TextureLoader::BridgeFormat
        // reads back; the layout is the channel convention the runtime sampler reads (NormalXY for
        // a two-channel normal whose Z it reconstructs).
        struct ResolvedFormat
        {
            TextureCodec Codec{};
            Renderer::Format Format{};
            CookedChannelLayout ChannelLayout = CookedChannelLayout::Direct;
        };

        // Lowers a CompressionFormat (the closed codec-output set a role table holds) to the cook's
        // codec + header Format ordinal + channel layout, for an eight-bit source. RGBA16Sfloat
        // carries more than eight bits a channel, so an eight-bit source resolving to it is an
        // error here; a float source never reaches this function. @p role carries the channel
        // intent the format alone cannot: an ASTC (RGBA) codec resolving from a Normal role stores
        // X/Y and reconstructs Z, so it reports the NormalXY layout; BC5 is a native two-channel
        // normal codec and always reports it.
        Result<ResolvedFormat> ResolveCompressionFormat(CompressionFormat format,
                                                        CompressionRole role)
        {
            const CookedChannelLayout astcLayout = role == CompressionRole::Normal
                                                       ? CookedChannelLayout::NormalXY
                                                       : CookedChannelLayout::Direct;
            const Renderer::Format rendererFormat = ToRendererFormat(format);
            switch (format)
            {
            case CompressionFormat::RGBA8Unorm:
            case CompressionFormat::RGBA8Srgb:
                return ResolvedFormat{.Codec = TextureCodec::None, .Format = rendererFormat};
            case CompressionFormat::BC7Unorm:
            case CompressionFormat::BC7Srgb:
                return ResolvedFormat{.Codec = TextureCodec::BC7, .Format = rendererFormat};
            case CompressionFormat::BC5Unorm:
                return ResolvedFormat{.Codec = TextureCodec::BC5,
                                      .Format = rendererFormat,
                                      .ChannelLayout = CookedChannelLayout::NormalXY};
            case CompressionFormat::BC4Unorm:
                return ResolvedFormat{.Codec = TextureCodec::BC4, .Format = rendererFormat};
            case CompressionFormat::ASTC4x4Unorm:
            case CompressionFormat::ASTC4x4Srgb:
                return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                      .Format = rendererFormat,
                                      .ChannelLayout = astcLayout};
            case CompressionFormat::RGBA16Sfloat:
                return std::unexpected(
                    string("an LDR source cannot fill an HDR role; author the source as EXR or "
                           "16-bit PNG"));
            }
            return std::unexpected(string("unmapped CompressionFormat"));
        }

        // The half-float header Format storing @p channels channels. The half-float family's
        // member is picked by the source's width, so a one- or two-channel source stores only what
        // it carries.
        Renderer::Format HalfFormatFor(u32 channels)
        {
            switch (channels)
            {
            case 1:
                return Renderer::Format::R16Sfloat;
            case 2:
                return Renderer::Format::RG16Sfloat;
            default:
                return Renderer::Format::RGBA16Sfloat;
            }
        }

        // The codec + format a raw "compression" codec name pins, keyed off the texture's sRGB
        // flag for its sRGB-aware format pair and off @p channels for the half-float family. This
        // is the escape-hatch path: it wins over the role.
        ResolvedFormat RawCodecFormat(TextureCodec codec, bool srgb, u32 channels)
        {
            switch (codec)
            {
            case TextureCodec::Half:
                return ResolvedFormat{.Codec = TextureCodec::Half,
                                      .Format = HalfFormatFor(channels)};
            case TextureCodec::ASTC:
                return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                      .Format = srgb ? Renderer::Format::ASTC4x4Srgb
                                                     : Renderer::Format::ASTC4x4Unorm};
            case TextureCodec::BC7:
                return ResolvedFormat{.Codec = TextureCodec::BC7,
                                      .Format = srgb ? Renderer::Format::BC7Srgb
                                                     : Renderer::Format::BC7Unorm};
            case TextureCodec::BC5:
                return ResolvedFormat{.Codec = TextureCodec::BC5,
                                      .Format = Renderer::Format::BC5Unorm};
            case TextureCodec::BC4:
                return ResolvedFormat{.Codec = TextureCodec::BC4,
                                      .Format = Renderer::Format::BC4Unorm};
            case TextureCodec::None:
                return ResolvedFormat{.Codec = TextureCodec::None,
                                      .Format = srgb ? Renderer::Format::RGBA8Srgb
                                                     : Renderer::Format::RGBA8Unorm};
            }
            return ResolvedFormat{.Codec = TextureCodec::ASTC,
                                  .Format = srgb ? Renderer::Format::ASTC4x4Srgb
                                                 : Renderer::Format::ASTC4x4Unorm};
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

        // ASTC 4x4 quality preset, expressed as an astcenc effort level in [0, 100]. The encode is
        // the dominant cost of a cold cook, so this sits at ASTCENC_PRE_FAST (30) rather than the
        // encoder's mid preset: the block size is fixed either way, so effort buys block quality
        // and costs cook time, and nothing else. The codec is the per-architecture SIMD build
        // (NEON on Apple Silicon, SSE4.1 on x86_64 — see cooker/CMakeLists.txt), built with
        // ASTCENC_INVARIANCE off, so the blocks depend on the thread count and two fresh cooks are
        // not byte-identical. The effort level also selects *which* blocks are produced, so the
        // smoke golden is regenerated whenever either moves.
        // (The ASTCENC_PRE_* presets are static const float, not constexpr.)
        const f32 AstcQuality = ASTCENC_PRE_FAST;

        // astcenc rebuilds two process-global trigonometric tables on *every* astcenc_context_alloc,
        // not once, so allocating a context while another thread encodes races with that thread's
        // reads of them. Two things together make that safe without serializing the encode: a context
        // is cached per thread and reused (so the whole cook allocates a handful), and this lock
        // orders every allocation against every encode — exclusive to allocate, shared to compress.
        std::shared_mutex g_AstcTableLock;

        // One thread's reusable encoder context. Held in a thread_local, so it is freed when the
        // worker that allocated it exits.
        struct AstcContextCache
        {
            astcenc_profile Profile{};
            u32 ThreadCount = 0;
            astcenc_context* Context = nullptr;

            ~AstcContextCache()
            {
                if (Context != nullptr)
                {
                    astcenc_context_free(Context);
                }
            }
        };

        // Returns this thread's encoder context for (@p profile, @p threadCount), allocating one
        // only when the cached context does not match. A reused context is reset, which is the
        // encoder's documented way to start a new image on an existing context.
        Result<astcenc_context*> AcquireAstcContext(astcenc_profile profile, u32 threadCount)
        {
            thread_local AstcContextCache cache;
            if (cache.Context != nullptr && cache.Profile == profile &&
                cache.ThreadCount == threadCount)
            {
                astcenc_compress_reset(cache.Context);
                return cache.Context;
            }

            astcenc_context* context = nullptr;
            {
                const std::unique_lock tables(g_AstcTableLock);
                astcenc_config config{};
                const astcenc_error configStatus =
                    astcenc_config_init(profile, BlockSize, BlockSize, 1, AstcQuality,
                                        ASTCENC_FLG_USE_DECODE_UNORM8, &config);
                if (configStatus != ASTCENC_SUCCESS)
                {
                    return std::unexpected(
                        fmt::format("texture importer: ASTC config init failed: {}",
                                    astcenc_get_error_string(configStatus)));
                }

                const astcenc_error allocStatus =
                    astcenc_context_alloc(&config, threadCount, &context);
                if (allocStatus != ASTCENC_SUCCESS)
                {
                    return std::unexpected(
                        fmt::format("texture importer: ASTC context alloc failed: {}",
                                    astcenc_get_error_string(allocStatus)));
                }
            }

            if (cache.Context != nullptr)
            {
                astcenc_context_free(cache.Context);
            }
            cache.Profile = profile;
            cache.ThreadCount = threadCount;
            cache.Context = context;
            return context;
        }

        // Encodes one RGBA8 mip level (tightly packed, row-major) to ASTC 4x4 LDR blocks through the
        // ARM astc-encoder. The encoder pads partial edge tiles internally, so the full chain down
        // to 1x1 encodes. @p profile selects the sRGB-aware LDR profile.
        //
        // The level's blocks are encoded across @p threadBudget threads — the encoder's documented
        // model: one context configured for N threads, then N calls to astcenc_compress_image, each
        // from a distinct thread under its own [0..N-1] index, with the blocks dynamically scheduled
        // across them. Under the cook's asset pool the budget is one, so the level encodes on the
        // calling thread and the parallelism comes from the pool instead.
        Result<vector<u8>> EncodeAstcLevel(u8* rgba, u32 width, u32 height, astcenc_profile profile,
                                           u32 threadBudget)
        {
            const u32 blocksWide = (width + BlockSize - 1) / BlockSize;
            const u32 blocksHigh = (height + BlockSize - 1) / BlockSize;
            const u32 blockCount = blocksWide * blocksHigh;

            // Cap the worker count at the level's block count so a tiny mip does not spawn idle
            // threads (the encoder schedules whole blocks, never a fraction of one), and at the
            // cook's shared budget so the encode does not stack a pool on the driver's workers.
            const u32 threadCount = std::min(std::max(1u, threadBudget), std::max(1u, blockCount));

            const Result<astcenc_context*> acquired = AcquireAstcContext(profile, threadCount);
            if (!acquired)
            {
                return std::unexpected(acquired.error());
            }
            astcenc_context* context = *acquired;

            void* slice = rgba;
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
            // the calling thread while the rest run on spawned workers, all joined before the shared
            // lock is dropped.
            vector<astcenc_error> statuses(threadCount, ASTCENC_SUCCESS);
            const std::shared_lock tables(g_AstcTableLock);
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

        // BC4 packs one channel into an 8-byte block; BC5 packs two channels into a 16-byte block.
        constexpr usize Bc4BlockBytes = 8;
        constexpr usize Bc5BlockBytes = 16;

        // Builds the padded 4x4x4 RGBA source block at (bx, by), replicating the nearest in-bounds
        // texel on a partial edge tile — the shared block gather the BC4/BC5/BC7 encoders read. The
        // standard edge-padding for block compression: a non-multiple-of-4 level encodes whole.
        std::array<u8, BlockSize * BlockSize * 4> GatherBlock(const u8* rgba, u32 width, u32 height,
                                                              u32 bx, u32 by)
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
            return source;
        }

        // Encodes one RGBA8 mip level (tightly packed, row-major) to BC7 blocks.
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
                    const std::array<u8, BlockSize * BlockSize * 4> source =
                        GatherBlock(rgba, width, height, bx, by);
                    const usize blockIndex = static_cast<usize>(by) * blocksWide + bx;
                    bc7enc_compress_block(blocks.data() + blockIndex * BlockBytes, source.data(),
                                          &params);
                }
            }

            return blocks;
        }

        // Encodes one RGBA8 mip level to BC5 blocks (two-channel RG, 16 bytes/block): the X/Y of a
        // tangent-space normal map, the Z dropped and reconstructed in-shader. rgbcx reads channels
        // 0 and 1 of each gathered 4x4 RGBA block.
        vector<u8> EncodeBc5Level(const u8* rgba, u32 width, u32 height)
        {
            const u32 blocksWide = (width + BlockSize - 1) / BlockSize;
            const u32 blocksHigh = (height + BlockSize - 1) / BlockSize;

            vector<u8> blocks(static_cast<usize>(blocksWide) * blocksHigh * Bc5BlockBytes);

            for (u32 by = 0; by < blocksHigh; by++)
            {
                for (u32 bx = 0; bx < blocksWide; bx++)
                {
                    const std::array<u8, BlockSize * BlockSize * 4> source =
                        GatherBlock(rgba, width, height, bx, by);
                    const usize blockIndex = static_cast<usize>(by) * blocksWide + bx;
                    rgbcx::encode_bc5(blocks.data() + blockIndex * Bc5BlockBytes, source.data(), 0,
                                      1, 4);
                }
            }

            return blocks;
        }

        // Encodes one RGBA8 mip level to BC4 blocks (single-channel R, 8 bytes/block): the R channel
        // of a mask map. rgbcx reads channel 0 of each gathered 4x4 RGBA block.
        vector<u8> EncodeBc4Level(const u8* rgba, u32 width, u32 height)
        {
            const u32 blocksWide = (width + BlockSize - 1) / BlockSize;
            const u32 blocksHigh = (height + BlockSize - 1) / BlockSize;

            vector<u8> blocks(static_cast<usize>(blocksWide) * blocksHigh * Bc4BlockBytes);

            for (u32 by = 0; by < blocksHigh; by++)
            {
                for (u32 bx = 0; bx < blocksWide; bx++)
                {
                    const std::array<u8, BlockSize * BlockSize * 4> source =
                        GatherBlock(rgba, width, height, bx, by);
                    const usize blockIndex = static_cast<usize>(by) * blocksWide + bx;
                    rgbcx::encode_bc4(blocks.data() + blockIndex * Bc4BlockBytes, source.data(), 4);
                }
            }

            return blocks;
        }

        // The largest magnitude a half can represent; a float beyond it clamps here rather than
        // packing an infinity the sampler would spread across a filtered neighbourhood.
        constexpr f32 HalfMax = 65504.0f;

        // Converts one f32 to a half's bit pattern, rounding to nearest with ties to even. A
        // magnitude past the half range clamps to +/-HalfMax and a NaN packs as zero; either sets
        // @p outOfRange so the caller can report the source once.
        u16 PackHalf(f32 value, bool& outOfRange)
        {
            if (std::isnan(value))
            {
                outOfRange = true;
                return 0;
            }
            if (value > HalfMax)
            {
                outOfRange = true;
                value = HalfMax;
            }
            else if (value < -HalfMax)
            {
                outOfRange = true;
                value = -HalfMax;
            }

            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const u32 sign = (bits >> 16) & 0x8000u;
            const i32 exponent = static_cast<i32>((bits >> 23) & 0xFFu) - 127 + 15;
            const u32 mantissa = bits & 0x007FFFFFu;

            if (exponent <= 0)
            {
                // Subnormal half: restore the float's implicit leading one and shift the
                // significand down to the fixed 2^-24 grid. Below that grid the value rounds to
                // zero, and a shift that far would be undefined.
                if (exponent < -10)
                {
                    return static_cast<u16>(sign);
                }
                const u32 significand = mantissa | 0x00800000u;
                const u32 shift = static_cast<u32>(14 - exponent);
                u32 result = significand >> shift;
                const u32 remainder = significand & ((1u << shift) - 1u);
                const u32 tie = 1u << (shift - 1);
                if (remainder > tie || (remainder == tie && (result & 1u) != 0))
                {
                    result++;
                }
                return static_cast<u16>(sign | result);
            }

            // Normal half. The clamp above keeps the exponent at or below 30, so rounding up can
            // reach 0x7BFF (HalfMax) but never the infinity encoding.
            u32 result = (static_cast<u32>(exponent) << 10) | (mantissa >> 13);
            const u32 remainder = mantissa & 0x1FFFu;
            if (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0))
            {
                result++;
            }
            return static_cast<u16>(sign | result);
        }

        // A decoded texture source. An eight-bit source fills Bytes with RGBA8 texels; a float
        // source (OpenEXR, or a sixteen-bit PNG) fills Texels with RGBA f32 ones. Channels is what
        // the file carries, narrowed to the 1 / 2 / 4 widths a cooked texture stores.
        struct DecodedSource
        {
            vector<u8> Bytes;
            vector<f32> Texels;
            int Width = 0;
            int Height = 0;
            u32 Channels = 4;
            bool Float = false;
        };

        // The channel count an OpenEXR file carries, narrowed to a cooked width. LoadEXR decodes a
        // one-channel file (replicated across RGBA) or an RGB(A) file and refuses anything else,
        // and reports no channel information of its own, so the header is parsed separately for it.
        Result<u32> ExrChannelCount(const path& imagePath)
        {
            const string file = imagePath.string();

            EXRVersion version{};
            if (ParseEXRVersionFromFile(&version, file.c_str()) != TINYEXR_SUCCESS)
            {
                return std::unexpected(fmt::format("'{}' is not a valid OpenEXR file", file));
            }

            EXRHeader header;
            InitEXRHeader(&header);
            const char* exrError = nullptr;
            if (ParseEXRHeaderFromFile(&header, &version, file.c_str(), &exrError) !=
                TINYEXR_SUCCESS)
            {
                const string message =
                    exrError != nullptr ? string(exrError) : string("unknown tinyexr error");
                FreeEXRErrorMessage(exrError);
                return std::unexpected(
                    fmt::format("failed to parse the OpenEXR header of '{}': {}", file, message));
            }

            const int channels = header.num_channels;
            FreeEXRHeader(&header);
            return channels == 1 ? 1u : 4u;
        }

        // Decodes an OpenEXR image to RGBA f32; tinyexr allocates the buffer, freed here.
        Result<DecodedSource> DecodeExr(const path& imagePath)
        {
            const Result<u32> channels = ExrChannelCount(imagePath);
            if (!channels)
            {
                return std::unexpected(channels.error());
            }

            float* pixels = nullptr;
            int width = 0;
            int height = 0;
            const char* exrError = nullptr;
            if (LoadEXR(&pixels, &width, &height, imagePath.string().c_str(), &exrError) !=
                TINYEXR_SUCCESS)
            {
                const string message =
                    exrError != nullptr ? string(exrError) : string("unknown tinyexr error");
                FreeEXRErrorMessage(exrError);
                return std::unexpected(
                    fmt::format("failed to load '{}': {}", imagePath.string(), message));
            }

            DecodedSource decoded{
                .Width = width,
                .Height = height,
                .Channels = *channels,
                .Float = true,
            };
            decoded.Texels.assign(pixels, pixels + static_cast<usize>(width) * height * 4);
            std::free(pixels);
            return decoded;
        }

        // Decodes a sixteen-bit image to RGBA f32, scaling each u16 by 1/65535. The source's own
        // channels are expanded explicitly rather than through stb's four-channel request, so a
        // two-channel file's second channel lands in G instead of being replicated as luminance.
        Result<DecodedSource> DecodeSixteenBit(const path& imagePath)
        {
            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_us* pixels =
                stbi_load_16(imagePath.string().c_str(), &width, &height, &channels, 0);
            if (pixels == nullptr)
            {
                return std::unexpected(fmt::format("failed to load image '{}': {}",
                                                   imagePath.string(), stbi_failure_reason()));
            }

            const usize texelCount = static_cast<usize>(width) * height;
            DecodedSource decoded{
                .Width = width,
                .Height = height,
                .Channels = channels <= 1 ? 1u : (channels == 2 ? 2u : 4u),
                .Float = true,
            };
            decoded.Texels.assign(texelCount * 4, 0.0f);
            constexpr f32 Inv16 = 1.0f / 65535.0f;
            for (usize texel = 0; texel < texelCount; texel++)
            {
                f32* out = decoded.Texels.data() + texel * 4;
                const stbi_us* in = pixels + texel * static_cast<usize>(channels);
                for (int c = 0; c < channels && c < 4; c++)
                {
                    out[c] = static_cast<f32>(in[c]) * Inv16;
                }
                if (channels < 4)
                {
                    out[3] = 1.0f;
                }
            }
            stbi_image_free(pixels);
            return decoded;
        }

        // Lowercases an ASCII string, for a case-insensitive file-extension match.
        string ToLowerAscii(string value)
        {
            std::ranges::transform(value, value.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        // Decodes a texture source. An OpenEXR file, or any image stb reports as sixteen-bit,
        // takes the float path; everything else decodes to RGBA8.
        Result<DecodedSource> DecodeSource(const path& imagePath)
        {
            if (ToLowerAscii(imagePath.extension().string()) == ".exr")
            {
                return DecodeExr(imagePath);
            }
            if (stbi_is_16_bit(imagePath.string().c_str()) != 0)
            {
                return DecodeSixteenBit(imagePath);
            }

            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load(imagePath.string().c_str(), &width, &height, &channels, 4);
            if (pixels == nullptr)
            {
                return std::unexpected(fmt::format("failed to load image '{}': {}",
                                                   imagePath.string(), stbi_failure_reason()));
            }

            DecodedSource decoded{.Width = width, .Height = height};
            decoded.Bytes.assign(pixels, pixels + static_cast<usize>(width) * height * 4);
            stbi_image_free(pixels);
            return decoded;
        }

        // Halves an RGBA f32 level with a 2x2 box filter. An odd edge clamps to the level's last
        // column or row, so its outermost texels contribute rather than being dropped.
        vector<f32> HalveFloatLevel(const vector<f32>& source, u32 width, u32 height, u32 outWidth,
                                    u32 outHeight)
        {
            vector<f32> result(static_cast<usize>(outWidth) * outHeight * 4);
            for (u32 y = 0; y < outHeight; y++)
            {
                const u32 y0 = std::min(y * 2, height - 1);
                const u32 y1 = std::min(y * 2 + 1, height - 1);
                for (u32 x = 0; x < outWidth; x++)
                {
                    const u32 x0 = std::min(x * 2, width - 1);
                    const u32 x1 = std::min(x * 2 + 1, width - 1);
                    const usize taps[4] = {
                        (static_cast<usize>(y0) * width + x0) * 4,
                        (static_cast<usize>(y0) * width + x1) * 4,
                        (static_cast<usize>(y1) * width + x0) * 4,
                        (static_cast<usize>(y1) * width + x1) * 4,
                    };
                    f32* out = result.data() + (static_cast<usize>(y) * outWidth + x) * 4;
                    for (u32 c = 0; c < 4; c++)
                    {
                        out[c] = (source[taps[0] + c] + source[taps[1] + c] + source[taps[2] + c] +
                                  source[taps[3] + c]) *
                                 0.25f;
                    }
                }
            }
            return result;
        }

        // Thin ordinal adapters over the shared authoring-name tables
        // (Renderer/TypeNames.h): CookedTextureHeader stores the enum ordinals as u32.

        optional<u32> ParseFilterOrdinal(const string& name)
        {
            const optional<Renderer::Filter> filter = Renderer::ParseFilter(name);
            return filter ? optional<u32>{static_cast<u32>(*filter)} : std::nullopt;
        }

        optional<u32> ParseMipmapModeOrdinal(const string& name)
        {
            const optional<Renderer::MipmapMode> mode = Renderer::ParseMipmapMode(name);
            return mode ? optional<u32>{static_cast<u32>(*mode)} : std::nullopt;
        }

        optional<u32> ParseAddressModeOrdinal(const string& name)
        {
            const optional<Renderer::AddressMode> mode = Renderer::ParseAddressMode(name);
            return mode ? optional<u32>{static_cast<u32>(*mode)} : std::nullopt;
        }
    }

    Result<vector<u8>> TextureImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("texture importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<json> texJsonResult = ReadJsonFile(sourcePath, "texture importer");
        if (!texJsonResult)
        {
            return std::unexpected(texJsonResult.error());
        }
        const json& texJson = *texJsonResult;

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
                            "'ASTC', 'BC7', 'None', or 'RGBA16F')",
                            sourcePath.string(), texJson["compression"].get<string>()));
        }

        CompressionRole role = srgb ? CompressionRole::Color : CompressionRole::Mask;
        if (texJson.contains("role") && texJson["role"].is_string())
        {
            const string roleName = texJson["role"].get<string>();
            const optional<CompressionRole> parsed = ParseCompressionRole(roleName);
            if (!parsed)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': invalid role '{}' (expected "
                                "'Color', 'Normal', 'Mask', 'HDR', or 'UI')",
                                sourcePath.string(), roleName));
            }
            role = *parsed;
        }

        // "channels" narrows a float source's stored width: a source wider than the data it
        // carries stores only R, or R and G, and the rest is simply not written.
        optional<u32> pinnedChannels;
        if (texJson.contains("channels"))
        {
            const u32 value =
                texJson["channels"].is_number_unsigned() ? texJson["channels"].get<u32>() : 0u;
            if (value != 1 && value != 2 && value != 4)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': invalid channels '{}' (expected 1, 2, "
                                "or 4)",
                                sourcePath.string(), texJson["channels"].dump()));
            }
            pinnedChannels = value;
        }

        const path imagePath = sourcePath.parent_path() / texJson["image"].get<string>();
        context.RecordDependency(imagePath);

        const Result<DecodedSource> decodedResult = DecodeSource(imagePath);
        if (!decodedResult)
        {
            return std::unexpected(fmt::format("texture importer: '{}': {}", sourcePath.string(),
                                               decodedResult.error()));
        }
        const DecodedSource& decoded = *decodedResult;

        // A float source has no eight-bit fallback: it resolves to the half-float family, whose
        // member the source's own width picks. Everything an eight-bit encoder could do to it
        // would quantise the range it was authored to carry, so the alternatives are refused.
        ResolvedFormat resolved{};
        u32 storedChannels = 4;
        if (decoded.Float)
        {
            if (srgb)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': \"srgb\": true on a float source; a "
                                "float texture is linear",
                                sourcePath.string()));
            }
            if (rawCodec && *rawCodec != TextureCodec::Half)
            {
                return std::unexpected(fmt::format(
                    "texture importer: '{}': compression '{}' cannot encode a float source; "
                    "declare \"compression\": \"RGBA16F\" or \"role\": \"HDR\"",
                    sourcePath.string(), texJson["compression"].get<string>()));
            }
            if (!rawCodec && context.Config != nullptr)
            {
                const CompressionFormat roleFormat = context.Config->Formats.GetFormat(role);
                if (roleFormat != CompressionFormat::RGBA16Sfloat)
                {
                    return std::unexpected(fmt::format(
                        "texture importer: '{}': a float source at role '{}' resolves to {}, "
                        "which cannot carry float data; declare \"role\": \"HDR\"",
                        sourcePath.string(), ToString(role), ToString(roleFormat)));
                }
            }
            storedChannels = pinnedChannels.value_or(decoded.Channels);
            resolved = RawCodecFormat(TextureCodec::Half, srgb, storedChannels);
        }
        else
        {
            if (pinnedChannels)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': \"channels\" narrows a float source; an "
                                "8-bit source always stores RGBA",
                                sourcePath.string()));
            }
            if (rawCodec == TextureCodec::Half)
            {
                return std::unexpected(
                    fmt::format("texture importer: '{}': an LDR source cannot fill an HDR role; "
                                "author the source as EXR or 16-bit PNG",
                                sourcePath.string()));
            }

            if (rawCodec)
            {
                resolved = RawCodecFormat(*rawCodec, srgb, 4);
            }
            else if (context.Config != nullptr)
            {
                const CompressionFormat roleFormat = context.Config->Formats.GetFormat(role);
                const Result<ResolvedFormat> lowered = ResolveCompressionFormat(roleFormat, role);
                if (!lowered)
                {
                    return std::unexpected(fmt::format("texture importer: '{}': {}",
                                                       sourcePath.string(), lowered.error()));
                }
                resolved = *lowered;
            }
            else
            {
                // Zero-config default: the hardcoded ASTC codec, the Metal-blessed codec on the
                // primary platform, preserved bit-for-bit for an un-migrated project.
                resolved = RawCodecFormat(TextureCodec::ASTC, srgb, 4);
                // ASTC has no two-channel mode; a Normal role still stores X/Y and reconstructs Z.
                resolved.ChannelLayout = role == CompressionRole::Normal
                                             ? CookedChannelLayout::NormalXY
                                             : CookedChannelLayout::Direct;
            }
        }

        const TextureCodec codec = resolved.Codec;

        // The resolved format is authoritative for sRGB-ness: it carries the role's intent, so a
        // config-driven Color → *Srgb encodes (and resizes) in gamma space regardless of the source
        // "srgb" flag. The override and zero-config formats are keyed off "srgb", so this equals the
        // flag on those paths — the encode stays byte-identical.
        const bool srgbEncode = resolved.Format == Renderer::Format::RGBA8Srgb ||
                                resolved.Format == Renderer::Format::BC7Srgb ||
                                resolved.Format == Renderer::Format::ASTC4x4Srgb;

        const int width = decoded.Width;
        const int height = decoded.Height;

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

        const bool resizing = targetWidth != width || targetHeight != height;
        const usize targetTexels =
            static_cast<usize>(targetWidth) * static_cast<usize>(targetHeight);

        vector<u8> pixelData;
        vector<f32> floatPixels;
        if (decoded.Float)
        {
            if (resizing)
            {
                floatPixels.assign(targetTexels * 4, 0.0f);
                if (stbir_resize_float_linear(decoded.Texels.data(), width, height, 0,
                                              floatPixels.data(), targetWidth, targetHeight, 0,
                                              STBIR_RGBA) == nullptr)
                {
                    return std::unexpected(
                        fmt::format("texture importer: '{}': failed to resize '{}'",
                                    sourcePath.string(), imagePath.string()));
                }
            }
            else
            {
                floatPixels = decoded.Texels;
            }
        }
        else
        {
            pixelData.assign(targetTexels * 4, 0);
            if (resizing)
            {
                const stbir_pixel_layout layout = STBIR_RGBA;
                const unsigned char* resized =
                    srgbEncode ? stbir_resize_uint8_srgb(decoded.Bytes.data(), width, height, 0,
                                                         pixelData.data(), targetWidth,
                                                         targetHeight, 0, layout)
                               : stbir_resize_uint8_linear(decoded.Bytes.data(), width, height, 0,
                                                           pixelData.data(), targetWidth,
                                                           targetHeight, 0, layout);
                if (resized == nullptr)
                {
                    return std::unexpected(
                        fmt::format("texture importer: '{}': failed to resize '{}'",
                                    sourcePath.string(), imagePath.string()));
                }
            }
            else
            {
                std::memcpy(pixelData.data(), decoded.Bytes.data(), targetTexels * 4);
            }
        }

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
        header.Format = static_cast<u32>(resolved.Format);
        header.Width = baseWidth;
        header.Height = baseHeight;
        header.MipCount = mipCount;
        header.ChannelLayout = static_cast<u32>(resolved.ChannelLayout);
        header.Version = CookedTextureVersion;

        // The runtime's texture-quality mip cap may drop the top mips of a Color/Normal/HDR texture;
        // UI and mask/data textures keep every level (a capped atlas softens menus, a capped mask
        // corrupts data-like sampling). A single-mip texture has nothing to cap.
        const bool roleCappable = role == CompressionRole::Color ||
                                  role == CompressionRole::Normal || role == CompressionRole::HDR;
        header.MipCappable = (roleCappable && mipCount > 1) ? 1u : 0u;

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

            const Result<u32> minFilter = field("min", ParseFilterOrdinal, header.MinFilter);
            if (!minFilter)
            {
                return std::unexpected(minFilter.error());
            }
            header.MinFilter = *minFilter;

            const Result<u32> magFilter = field("mag", ParseFilterOrdinal, header.MagFilter);
            if (!magFilter)
            {
                return std::unexpected(magFilter.error());
            }
            header.MagFilter = *magFilter;

            const Result<u32> mipmapMode =
                field("mipmap", ParseMipmapModeOrdinal, header.MipmapMode);
            if (!mipmapMode)
            {
                return std::unexpected(mipmapMode.error());
            }
            header.MipmapMode = *mipmapMode;

            const Result<u32> addressModeU =
                field("wrap_u", ParseAddressModeOrdinal, header.AddressModeU);
            if (!addressModeU)
            {
                return std::unexpected(addressModeU.error());
            }
            header.AddressModeU = *addressModeU;

            const Result<u32> addressModeV =
                field("wrap_v", ParseAddressModeOrdinal, header.AddressModeV);
            if (!addressModeV)
            {
                return std::unexpected(addressModeV.error());
            }
            header.AddressModeV = *addressModeV;

            const Result<u32> addressModeW =
                field("wrap_w", ParseAddressModeOrdinal, header.AddressModeW);
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

        // The byte size of one mip level in the chosen codec: a BC4 4x4 block is 8 bytes; BC7,
        // BC5, and ASTC are 16-byte 4x4 blocks; Half is w*h*channels half-floats; None is
        // uncompressed RGBA8 (w*h*4). Mirrors the engine's BytesForLevel.
        const usize codecBlockBytes = codec == TextureCodec::BC4 ? Bc4BlockBytes : BlockBytes;
        const bool blockCodec = codec == TextureCodec::BC7 || codec == TextureCodec::ASTC ||
                                codec == TextureCodec::BC5 || codec == TextureCodec::BC4;
        const auto levelBytes = [blockCodec, codecBlockBytes, codec,
                                 storedChannels](u32 levelWidth, u32 levelHeight) -> usize
        {
            if (blockCodec)
            {
                const u32 blocksWide = (levelWidth + BlockSize - 1) / BlockSize;
                const u32 blocksHigh = (levelHeight + BlockSize - 1) / BlockSize;
                return static_cast<usize>(blocksWide) * blocksHigh * codecBlockBytes;
            }
            if (codec == TextureCodec::Half)
            {
                return static_cast<usize>(levelWidth) * levelHeight * storedChannels * sizeof(u16);
            }
            return static_cast<usize>(levelWidth) * levelHeight * 4;
        };

        // Encodes one RGBA8 level into the codec's on-disk bytes at blob[writeOffset]. BC7/BC5/BC4
        // and ASTC pack 4x4 blocks; None copies the RGBA8 bytes through unchanged. The ASTC profile
        // is the sRGB-aware LDR profile matching the chosen format pair.
        bc7enc_compress_block_params params{};
        if (codec == TextureCodec::BC7)
        {
            // bc7enc_compress_block_init fills process-wide encode tables, so two concurrent
            // texture cooks would write them at once; call_once makes the fill happen exactly
            // once and every later reader see it. params_init writes only the local struct.
            static std::once_flag bc7Init;
            std::call_once(bc7Init, [] { bc7enc_compress_block_init(); });
            bc7enc_compress_block_params_init(&params);
            params.m_uber_level = BC7UberLevel;
        }
        // rgbcx backs both BC4 and BC5; init builds its shared block-encode tables, likewise
        // process-wide and likewise filled exactly once.
        if (codec == TextureCodec::BC5 || codec == TextureCodec::BC4)
        {
            static std::once_flag rgbcxInit;
            std::call_once(rgbcxInit, [] { rgbcx::init(); });
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
            else if (codec == TextureCodec::BC5)
            {
                const vector<u8> blocks = EncodeBc5Level(rgba, lw, lh);
                std::memcpy(blob.data() + writeOffset, blocks.data(), blocks.size());
            }
            else if (codec == TextureCodec::BC4)
            {
                const vector<u8> blocks = EncodeBc4Level(rgba, lw, lh);
                std::memcpy(blob.data() + writeOffset, blocks.data(), blocks.size());
            }
            else if (codec == TextureCodec::ASTC)
            {
                const Result<vector<u8>> blocks =
                    EncodeAstcLevel(rgba, lw, lh, astcProfile, context.ThreadBudget);
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

        // A float chain filters in f32 and converts each level to half afterwards, so no level is
        // filtered from a quantised parent.
        if (codec == TextureCodec::Half)
        {
            bool outOfRange = false;
            vector<f32> level = std::move(floatPixels);
            u32 levelWidth = baseWidth;
            u32 levelHeight = baseHeight;
            for (u32 index = 0; index < mipCount; index++)
            {
                if (index > 0)
                {
                    const u32 nextWidth = std::max(1u, levelWidth >> 1);
                    const u32 nextHeight = std::max(1u, levelHeight >> 1);
                    level = HalveFloatLevel(level, levelWidth, levelHeight, nextWidth, nextHeight);
                    levelWidth = nextWidth;
                    levelHeight = nextHeight;
                }

                auto* out = reinterpret_cast<u16*>(blob.data() + writeOffset);
                const usize texels = static_cast<usize>(levelWidth) * levelHeight;
                for (usize texel = 0; texel < texels; texel++)
                {
                    for (u32 channel = 0; channel < storedChannels; channel++)
                    {
                        out[texel * storedChannels + channel] =
                            PackHalf(level[texel * 4 + channel], outOfRange);
                    }
                }
                writeOffset += levelBytes(levelWidth, levelHeight);
            }

            if (outOfRange)
            {
                fmt::print(stderr,
                           "texture importer: '{}': '{}' carries values outside the half range; "
                           "they are clamped to +/-{}\n",
                           sourcePath.string(), imagePath.string(), HalfMax);
            }

            return blob;
        }

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
