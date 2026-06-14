#include "TextureLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    namespace
    {
        // The Bridge* helpers below map the cooked header's underlying-integer
        // enum fields to their Veng::Renderer enums — the engine side of the
        // cycle-avoidance rule documented in assetformat's CookedBlobs.h. An
        // unrecognized value means a stale/corrupt cooked archive, hence
        // AssetError::Corrupt (recoverable) rather than VE_ASSERT.

        optional<Renderer::Format> BridgeFormat(u32 value)
        {
            switch (value)
            {
                case 2: return Renderer::Format::RGBA8Unorm;
                case 3: return Renderer::Format::RGBA8Srgb;
                default: return std::nullopt;
            }
        }

        optional<Renderer::Filter> BridgeFilter(u32 value)
        {
            switch (value)
            {
                case 0: return Renderer::Filter::Nearest;
                case 1: return Renderer::Filter::Linear;
                default: return std::nullopt;
            }
        }

        optional<Renderer::MipmapMode> BridgeMipmapMode(u32 value)
        {
            switch (value)
            {
                case 0: return Renderer::MipmapMode::Nearest;
                case 1: return Renderer::MipmapMode::Linear;
                default: return std::nullopt;
            }
        }

        optional<Renderer::AddressMode> BridgeAddressMode(u32 value)
        {
            switch (value)
            {
                case 0: return Renderer::AddressMode::Repeat;
                case 1: return Renderer::AddressMode::MirroredRepeat;
                case 2: return Renderer::AddressMode::ClampToEdge;
                case 3: return Renderer::AddressMode::ClampToBorder;
                default: return std::nullopt;
            }
        }
    }

    AssetResult<Detail::LoadJob> TextureLoader::Load(
        AssetManager& /*manager*/, Renderer::Context& context, TaskSystem& tasks,
        AssetId id, std::span<const u8> cooked, bool async) const
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

        if (header.MipCount != 1)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = fmt::format("texture: unsupported MipCount {} (v1 is single-mip)", header.MipCount),
            });
        }

        const optional<Renderer::Format> format = BridgeFormat(header.Format);
        const optional<Renderer::Filter> minFilter = BridgeFilter(header.MinFilter);
        const optional<Renderer::Filter> magFilter = BridgeFilter(header.MagFilter);
        const optional<Renderer::MipmapMode> mipmapMode = BridgeMipmapMode(header.MipmapMode);
        const optional<Renderer::AddressMode> addressModeU = BridgeAddressMode(header.AddressModeU);
        const optional<Renderer::AddressMode> addressModeV = BridgeAddressMode(header.AddressModeV);
        const optional<Renderer::AddressMode> addressModeW = BridgeAddressMode(header.AddressModeW);

        if (!format || !minFilter || !magFilter || !mipmapMode || !addressModeU || !addressModeV || !addressModeW)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "texture: unrecognized format/sampler enum value in cooked header",
            });
        }

        const usize pixelBytes = static_cast<usize>(header.Width) * header.Height * 4;
        if (cooked.size() < sizeof(header) + pixelBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "texture: cooked blob smaller than header + pixel data",
            });
        }

        const TextureInfo info{
            .Name = fmt::format("Texture {}", id.Value),
            .Extent = {header.Width, header.Height},
            .Format = *format,
            .Pixels = cooked.subspan(sizeof(header), pixelBytes),
            .Sampler = {
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

        // Async: create the resource and record the upload on the transfer queue
        // (no device wait). Sync: create + blocking UploadSync. Either way the
        // texture comes back unregistered; Finalize registers it on the main
        // thread.
        Ref<Veng::Texture> texture;
        if (async)
        {
            Task<void> upload;
            texture = Veng::Texture::CreateAsync(context, info, tasks, upload);
        }
        else
        {
            texture = Veng::Texture::Create(context, info);
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(texture),
            .Dependencies = {},
            .Finalize = [texture]() -> VoidResult { texture->Finalize(); return {}; },
        };
    }
}
