#include "TextureLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    namespace
    {
        // The Bridge* helpers below map the cooked header's underlying-integer
        // enum fields to their Veng::Renderer enums — the engine side of the
        // cycle-avoidance rule documented in assetpack's CookedBlobs.h. An
        // unrecognized value means a stale/corrupt cooked archive, hence
        // AssetError::Corrupt (recoverable) rather than VE_ASSERT.

        optional<Renderer::Format> BridgeFormat(u32 value)
        {
            switch (value)
            {
            case 2:
                return Renderer::Format::RGBA8Unorm;
            case 3:
                return Renderer::Format::RGBA8Srgb;
            case 21:
                return Renderer::Format::BC7Unorm;
            case 22:
                return Renderer::Format::BC7Srgb;
            default:
                return std::nullopt;
            }
        }

        // True when the format is a BC block-compressed format, which requires the device's
        // textureCompressionBC feature to sample.
        bool IsBlockCompressed(Renderer::Format format)
        {
            return format == Renderer::Format::BC7Unorm || format == Renderer::Format::BC7Srgb;
        }

        optional<Renderer::Filter> BridgeFilter(u32 value)
        {
            switch (value)
            {
            case 0:
                return Renderer::Filter::Nearest;
            case 1:
                return Renderer::Filter::Linear;
            default:
                return std::nullopt;
            }
        }

        optional<Renderer::MipmapMode> BridgeMipmapMode(u32 value)
        {
            switch (value)
            {
            case 0:
                return Renderer::MipmapMode::Nearest;
            case 1:
                return Renderer::MipmapMode::Linear;
            default:
                return std::nullopt;
            }
        }

        optional<Renderer::AddressMode> BridgeAddressMode(u32 value)
        {
            switch (value)
            {
            case 0:
                return Renderer::AddressMode::Repeat;
            case 1:
                return Renderer::AddressMode::MirroredRepeat;
            case 2:
                return Renderer::AddressMode::ClampToEdge;
            case 3:
                return Renderer::AddressMode::ClampToBorder;
            default:
                return std::nullopt;
            }
        }
    }

    AssetResult<Detail::LoadJob> TextureLoader::Load(AssetManager& /*manager*/,
                                                     Renderer::Context& context, TaskSystem& tasks,
                                                     TypeRegistry& /*types*/, AssetId id,
                                                     std::span<const u8> cooked, bool async) const
    {
        if (cooked.size() < sizeof(CookedTextureHeader))
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "texture: cooked blob smaller than CookedTextureHeader",
            });
        }

        CookedTextureHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.MipCount < 1)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail =
                    fmt::format("texture: invalid MipCount {} (must be >= 1)", header.MipCount),
            });
        }

        const optional<Renderer::Format> format = BridgeFormat(header.Format);
        const optional<Renderer::Filter> minFilter = BridgeFilter(header.MinFilter);
        const optional<Renderer::Filter> magFilter = BridgeFilter(header.MagFilter);
        const optional<Renderer::MipmapMode> mipmapMode = BridgeMipmapMode(header.MipmapMode);
        const optional<Renderer::AddressMode> addressModeU = BridgeAddressMode(header.AddressModeU);
        const optional<Renderer::AddressMode> addressModeV = BridgeAddressMode(header.AddressModeV);
        const optional<Renderer::AddressMode> addressModeW = BridgeAddressMode(header.AddressModeW);

        if (!format || !minFilter || !magFilter || !mipmapMode || !addressModeU || !addressModeV ||
            !addressModeW)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "texture: unrecognized format/sampler enum value in cooked header",
            });
        }

        // A BC-cooked texture cannot be sampled on a device without textureCompressionBC, and the
        // runtime does not transcode — reject rather than crash or CPU-decode. Logged once so the
        // missing capability is observable without flooding the log per texture.
        if (IsBlockCompressed(*format) && !context.IsBlockCompressionSupported())
        {
            static bool s_BlockCompressionWarned = false;
            if (!s_BlockCompressionWarned)
            {
                Log::Warn("TextureLoader: a BC-compressed texture was cooked but the device lacks "
                          "textureCompressionBC; the texture is unsupported on this device.");
                s_BlockCompressionWarned = true;
            }
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Unsupported,
                .Id = id,
                .Detail = "texture: BC block compression unsupported on this device",
            });
        }

        // The mip levels are tightly packed largest-first; each level's size derives from its
        // halved dimensions through BytesForLevel (block-aware), so the total is a pure arithmetic
        // walk (no offset table) for both uncompressed and block-compressed formats.
        usize pixelBytes = 0;
        for (u32 level = 0; level < header.MipCount; level++)
        {
            const u32 levelWidth = std::max(1u, header.Width >> level);
            const u32 levelHeight = std::max(1u, header.Height >> level);
            pixelBytes += Renderer::BytesForLevel(*format, levelWidth, levelHeight);
        }

        if (cooked.size() < sizeof(header) + pixelBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "texture: cooked blob smaller than header + mip-level data",
            });
        }

        const TextureData info{
            .Name = fmt::format("Texture {}", id.Value),
            .Extent = {header.Width, header.Height},
            .Format = *format,
            .MipLevels = header.MipCount,
            .Pixels = cooked.subspan(sizeof(header), pixelBytes),
            .Sampler =
                {
                    .MagFilter = *magFilter,
                    .MinFilter = *minFilter,
                    .MipmapMode = *mipmapMode,
                    .AddressModeU = *addressModeU,
                    .AddressModeV = *addressModeV,
                    .AddressModeW = *addressModeW,
                    .AnisotropyEnabled = header.AnisotropyEnabled != 0,
                    .MaxAnisotropy = header.MaxAnisotropy,
                },
        };

        Ref<Veng::Texture> texture;
        if (async)
        {
            Task<void> upload;
            texture = Veng::Texture::PrepareAsync(context, info, tasks, upload);
        }
        else
        {
            texture = Veng::Texture::PrepareSync(context, info);
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(texture),
            .Dependencies = {},
            .Finalize = [texture]() -> VoidResult
            {
                texture->Finalize();
                return {};
            },
        };
    }
}
