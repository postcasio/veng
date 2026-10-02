#include <Veng/Asset/AssetLoader.h>

#include <fmt/format.h>

namespace Veng
{
    AssetResult<Detail::ParsedAsset> AssetLoader::Parse(const AssetParseContext& /*context*/,
                                                        const AssetId id,
                                                        std::span<const u8> /*cooked*/) const
    {
        return std::unexpected(AssetLoadError{
            .Kind = AssetError::LoadFailed,
            .Id = id,
            .Detail =
                fmt::format("the loader for asset {} is single-phase and has no Parse", id.Value),
        });
    }

    AssetResult<Detail::LoadJob> AssetLoader::Load(AssetManager& /*manager*/,
                                                   Renderer::Context& /*context*/,
                                                   TaskSystem& /*tasks*/, TypeRegistry& /*types*/,
                                                   const AssetId id, std::span<const u8> /*cooked*/,
                                                   bool /*async*/) const
    {
        return std::unexpected(AssetLoadError{
            .Kind = AssetError::LoadFailed,
            .Id = id,
            .Detail = fmt::format("the loader for asset {} is two-phase and has no Load", id.Value),
        });
    }
}
