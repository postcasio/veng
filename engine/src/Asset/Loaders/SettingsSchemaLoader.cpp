#include "SettingsSchemaLoader.h"

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
    SettingsSchemaLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                               TaskSystem& /*tasks*/, TypeRegistry& types, AssetId id,
                               std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedSettingsSchemaHeader))
        {
            return std::unexpected(Corrupt(
                id, "settings schema: cooked blob smaller than CookedSettingsSchemaHeader"));
        }

        CookedSettingsSchemaHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedSettingsSchemaVersion)
        {
            return std::unexpected(Corrupt(
                id,
                fmt::format("settings schema: blob version {} does not match expected version {}",
                            header.Version, CookedSettingsSchemaVersion)));
        }

        const usize cursor = sizeof(CookedSettingsSchemaHeader);
        if (cooked.size() < cursor + header.RecordBytes)
        {
            return std::unexpected(Corrupt(id, "settings schema: cooked blob truncated"));
        }

        const std::span<const u8> record = cooked.subspan(cursor, header.RecordBytes);

        SettingsSchemaData data;
        const VoidResult read =
            ReadFields(record, &data, types.Info(TypeIdOf<SettingsSchemaData>()), types);
        if (!read)
        {
            return std::unexpected(Corrupt(id, read.error()));
        }

        const Ref<SettingsSchema> schema = SettingsSchema::Create(std::move(data));
        return Detail::LoadJob{.Resource = Detail::RefAny(schema)};
    }
}
