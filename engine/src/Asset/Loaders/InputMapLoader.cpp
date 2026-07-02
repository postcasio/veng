#include "InputMapLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob> InputMapLoader::Load(AssetManager& /*manager*/,
                                                      Renderer::Context& /*context*/,
                                                      TaskSystem& /*tasks*/, TypeRegistry& types,
                                                      AssetId id, std::span<const u8> cooked,
                                                      bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedInputMapHeader))
        {
            return std::unexpected(
                Corrupt(id, "input map: cooked blob smaller than CookedInputMapHeader"));
        }

        CookedInputMapHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedInputMapVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("input map: blob version {} does not match expected version {}",
                                header.Version, CookedInputMapVersion)));
        }

        const usize cursor = sizeof(CookedInputMapHeader);
        if (cooked.size() < cursor + header.RecordBytes)
        {
            return std::unexpected(Corrupt(id, "input map: cooked blob truncated"));
        }

        const std::span<const u8> record = cooked.subspan(cursor, header.RecordBytes);

        InputMapData data;
        const VoidResult read =
            ReadFields(record, &data, types.Info(TypeIdOf<InputMapData>()), types);
        if (!read)
        {
            return std::unexpected(Corrupt(id, read.error()));
        }

        const Ref<InputMappingContext> context =
            InputMappingContext::Create(std::move(data.Actions), std::move(data.Bindings));

        return Detail::LoadJob{.Resource = Detail::RefAny(context)};
    }
}
