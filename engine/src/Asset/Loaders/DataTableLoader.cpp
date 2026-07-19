#include "DataTableLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Resolve the TableSchema handle (async or blocking), mirroring the material-instance parent.
        AssetResult<AssetHandle<TableSchema>> LoadSchema(AssetManager& manager, AssetId tableId,
                                                         u64 schemaId, bool async)
        {
            if (async)
            {
                AssetHandle<TableSchema> handle = manager.Load<TableSchema>(AssetId{schemaId});
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(
                        AssetLoadError{.Kind = AssetError::MissingDependency,
                                       .Id = AssetId{schemaId},
                                       .Detail = fmt::format("table {}: schema {} did not resolve",
                                                             tableId.Value, schemaId)});
                }
                return handle;
            }
            return manager.LoadSync<TableSchema>(AssetId{schemaId});
        }
    }

    AssetResult<Detail::LoadJob> DataTableLoader::Load(AssetManager& manager,
                                                       Renderer::Context& /*context*/,
                                                       TaskSystem& /*tasks*/,
                                                       TypeRegistry& /*types*/, AssetId id,
                                                       std::span<const u8> cooked, bool async) const
    {
        if (cooked.size() < sizeof(CookedDataTableHeader))
        {
            return std::unexpected(Corrupt(id, "data table: cooked blob smaller than header"));
        }

        CookedDataTableHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedDataTableVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("data table: blob version {} does not match version {} — "
                                        "re-cook the pack",
                                        header.Version, CookedDataTableVersion)));
        }
        if (header.KeyKind != static_cast<u32>(CookedTableColumnKind::Int) &&
            header.KeyKind != static_cast<u32>(CookedTableColumnKind::String))
        {
            return std::unexpected(
                Corrupt(id, fmt::format("data table: key kind {} is neither Int nor String",
                                        header.KeyKind)));
        }
        if (header.RowCount > 0 && header.RowStride == 0)
        {
            return std::unexpected(Corrupt(id, "data table: blob declares rows of zero bytes"));
        }

        usize cursor = sizeof(CookedDataTableHeader);
        const usize keyBytes = static_cast<usize>(header.RowCount) * sizeof(CookedTableKey);
        const usize rowBytes = static_cast<usize>(header.RowCount) * header.RowStride;
        if (cooked.size() < cursor + keyBytes + rowBytes + header.StringHeapBytes)
        {
            return std::unexpected(Corrupt(id, "data table: cooked blob truncated"));
        }

        vector<CookedTableKey> keys(header.RowCount);
        if (keyBytes > 0)
        {
            std::memcpy(keys.data(), cooked.data() + cursor, keyBytes);
        }
        cursor += keyBytes;

        for (const CookedTableKey& key : keys)
        {
            if (key.RowIndex >= header.RowCount)
            {
                return std::unexpected(
                    Corrupt(id, fmt::format("data table: key index addresses row {} of {}",
                                            key.RowIndex, header.RowCount)));
            }
        }

        vector<u8> rows(rowBytes);
        if (rowBytes > 0)
        {
            std::memcpy(rows.data(), cooked.data() + cursor, rowBytes);
        }
        cursor += rowBytes;

        vector<u8> stringHeap(header.StringHeapBytes);
        if (header.StringHeapBytes > 0)
        {
            std::memcpy(stringHeap.data(), cooked.data() + cursor, header.StringHeapBytes);
        }

        const AssetResult<AssetHandle<TableSchema>> schemaResult =
            LoadSchema(manager, id, header.SchemaId, async);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }

        const u32 rowStride = header.RowStride;
        const Ref<DataTable> table =
            DataTable::Create(*schemaResult, static_cast<TableColumnKind>(header.KeyKind),
                              rowStride, std::move(rows), std::move(keys), std::move(stringHeap));

        return Detail::LoadJob{
            .Resource = Detail::RefAny(table),
            .Dependencies = {AssetManager::EntryOf(*schemaResult)},
            // The schema is resident by the time Finalize runs, so this is where a table cooked
            // against a since-changed schema layout is caught rather than misread.
            .Finalize = [table, rowStride, id]() -> VoidResult
            {
                const u32 schemaStride = table->GetSchema().GetRowStride();
                if (schemaStride != rowStride)
                {
                    return std::unexpected(fmt::format(
                        "data table {}: rows are {} bytes but its schema lays out {} — re-cook "
                        "the pack",
                        id.Value, rowStride, schemaStride));
                }
                return {};
            },
        };
    }
}
