#include "GraphicsSchemaLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
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

    AssetResult<Detail::LoadJob>
    GraphicsSchemaLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                               TaskSystem& /*tasks*/, TypeRegistry& types, AssetId id,
                               std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedGraphicsSchemaHeader))
        {
            return std::unexpected(Corrupt(
                id, "graphics schema: cooked blob smaller than CookedGraphicsSchemaHeader"));
        }

        CookedGraphicsSchemaHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedGraphicsSchemaVersion)
        {
            return std::unexpected(Corrupt(
                id,
                fmt::format("graphics schema: blob version {} does not match expected version {}",
                            header.Version, CookedGraphicsSchemaVersion)));
        }

        const usize cursor = sizeof(CookedGraphicsSchemaHeader);
        if (cooked.size() < cursor + header.RecordBytes)
        {
            return std::unexpected(Corrupt(id, "graphics schema: cooked blob truncated"));
        }

        const std::span<const u8> record = cooked.subspan(cursor, header.RecordBytes);

        GraphicsSchemaData data;
        const VoidResult read =
            ReadFields(record, &data, types.Info(TypeIdOf<GraphicsSchemaData>()), types);
        if (!read)
        {
            return std::unexpected(Corrupt(id, read.error()));
        }

        const Ref<GraphicsSchema> schema = GraphicsSchema::Create(std::move(data));
        return Detail::LoadJob{.Resource = Detail::RefAny(schema)};
    }
}
