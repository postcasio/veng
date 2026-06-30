#include "EnvironmentLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    AssetResult<Detail::LoadJob> EnvironmentLoader::Load(AssetManager& /*manager*/,
                                                         Renderer::Context& context,
                                                         TaskSystem& tasks, TypeRegistry& /*types*/,
                                                         AssetId id, std::span<const u8> cooked,
                                                         bool async) const
    {
        if (cooked.size() < sizeof(CookedEnvironmentHeader))
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "environment: cooked blob smaller than CookedEnvironmentHeader",
            });
        }

        CookedEnvironmentHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedEnvironmentVersion)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = fmt::format("environment: blob version {} != expected {}", header.Version,
                                      CookedEnvironmentVersion),
            });
        }

        // The cooker emits RGBA16Sfloat (Renderer::Format ordinal 6); any other value is a
        // stale/corrupt blob.
        if (header.Format != 6u)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = fmt::format("environment: unsupported format ordinal {} (expected "
                                      "RGBA16Sfloat)",
                                      header.Format),
            });
        }

        // RGBA16Sfloat: four half-floats (8 bytes) per texel.
        const usize pixelBytes = static_cast<usize>(header.Width) * header.Height * 8;
        if (cooked.size() < sizeof(header) + pixelBytes)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt,
                .Id = id,
                .Detail = "environment: cooked blob smaller than header + pixel data",
            });
        }

        const EnvironmentMapData info{
            .Name = fmt::format("EnvironmentMap {}", id.Value),
            .Extent = {header.Width, header.Height},
            .Format = Renderer::Format::RGBA16Sfloat,
            .Pixels = cooked.subspan(sizeof(header), pixelBytes),
        };

        Ref<Veng::EnvironmentMap> environment;
        if (async)
        {
            Task<void> upload;
            environment = Veng::EnvironmentMap::PrepareAsync(context, info, tasks, upload);
        }
        else
        {
            environment = Veng::EnvironmentMap::PrepareSync(context, info);
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(environment),
            .Dependencies = {},
            .Finalize = [environment]() -> VoidResult
            {
                environment->Finalize();
                return {};
            },
        };
    }
}
