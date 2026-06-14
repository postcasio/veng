#include "RawAssetLoader.h"

namespace Veng
{
    AssetResult<Detail::RefAny> RawAssetLoader::Load(
        AssetManager& /*manager*/, Renderer::Context& /*context*/,
        AssetId /*id*/, std::span<const u8> cooked) const
    {
        return Detail::RefAny(CreateRef<RawAsset>(RawAsset{
            .Bytes = vector<u8>(cooked.begin(), cooked.end()),
        }));
    }
}
