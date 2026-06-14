#include "RawAssetLoader.h"

namespace Veng
{
    AssetResult<Detail::LoadJob> RawAssetLoader::Load(
        AssetManager& /*manager*/, Renderer::Context& /*context*/, TaskSystem& /*tasks*/,
        AssetId /*id*/, std::span<const u8> cooked, bool /*async*/) const
    {
        return Detail::LoadJob{
            .Resource = Detail::RefAny(CreateRef<RawAsset>(RawAsset{
                .Bytes = vector<u8>(cooked.begin(), cooked.end()),
            })),
        };
    }
}
