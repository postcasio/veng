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
                                                       TaskSystem& /*tasks*/, TypeRegistry& types,
                                                       AssetId id, std::span<const u8> cooked,
                                                       bool async) const
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
        if (header.KeyKind != static_cast<u32>(CookedTableKeyKind::Integer) &&
            header.KeyKind != static_cast<u32>(CookedTableKeyKind::String))
        {
            return std::unexpected(
                Corrupt(id, fmt::format("data table: key kind {} is neither Integer nor String",
                                        header.KeyKind)));
        }

        const bool fixedStride = header.FixedStride != 0;
        if (fixedStride && header.RowCount > 0 && header.RowStride == 0)
        {
            return std::unexpected(
                Corrupt(id, "data table: fixed-stride blob declares rows of zero bytes"));
        }

        const usize keyBytes = static_cast<usize>(header.RowCount) * sizeof(CookedTableKey);
        const usize directoryBytes =
            fixedStride ? 0 : static_cast<usize>(header.RowCount) * sizeof(u32);
        usize cursor = sizeof(CookedDataTableHeader);
        if (cooked.size() <
            cursor + keyBytes + directoryBytes + header.RowBytes + header.KeyHeapBytes)
        {
            return std::unexpected(Corrupt(id, "data table: cooked blob truncated"));
        }

        // --- The three length witnesses must agree before anything addresses a row ---
        //
        // The row count is reported off the key index, so a blob whose row region or directory
        // disagrees with it would read past (or short of) the rows it claims to hold. None of
        // these is API misuse: a bad blob on disk is a data condition, so each is Corrupt.

        if (fixedStride)
        {
            const usize impliedRows =
                header.RowStride == 0 ? 0 : header.RowBytes / header.RowStride;
            if (header.RowStride != 0 && header.RowBytes % header.RowStride != 0)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("data table: the {}-byte row region is not a whole number of "
                                    "{}-byte rows",
                                    header.RowBytes, header.RowStride)));
            }
            if (impliedRows != header.RowCount)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("data table: the key index holds {} rows but the row region "
                                    "holds {}",
                                    header.RowCount, impliedRows)));
            }
        }
        else if (header.RowCount == 0 && header.RowBytes != 0)
        {
            return std::unexpected(
                Corrupt(id, "data table: the key index holds no rows but the row region is "
                            "non-empty"));
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

        vector<u32> rowOffsets(directoryBytes / sizeof(u32));
        if (directoryBytes > 0)
        {
            std::memcpy(rowOffsets.data(), cooked.data() + cursor, directoryBytes);
        }
        cursor += directoryBytes;

        if (!fixedStride && header.RowCount > 0)
        {
            if (rowOffsets.front() != 0)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("data table: the row directory starts at offset {}, not 0",
                                    rowOffsets.front())));
            }
            for (u32 row = 1; row < header.RowCount; ++row)
            {
                // A non-monotonic directory cannot be trusted to yield in-bounds spans: each
                // row's extent is the gap to the next entry.
                if (rowOffsets[row] < rowOffsets[row - 1])
                {
                    return std::unexpected(Corrupt(
                        id, fmt::format("data table: the row directory steps backwards at row {} "
                                        "({} after {})",
                                        row, rowOffsets[row], rowOffsets[row - 1])));
                }
            }
            if (rowOffsets.back() > header.RowBytes)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("data table: the row directory's last entry is at {}, past "
                                    "the {}-byte row region",
                                    rowOffsets.back(), header.RowBytes)));
            }
        }

        vector<u8> rows(header.RowBytes);
        if (header.RowBytes > 0)
        {
            std::memcpy(rows.data(), cooked.data() + cursor, header.RowBytes);
        }
        cursor += header.RowBytes;

        vector<u8> keyHeap(header.KeyHeapBytes);
        if (header.KeyHeapBytes > 0)
        {
            std::memcpy(keyHeap.data(), cooked.data() + cursor, header.KeyHeapBytes);
        }

        for (const CookedTableKey& key : keys)
        {
            if (static_cast<usize>(key.StringKey.Offset) + key.StringKey.Length > keyHeap.size())
            {
                return std::unexpected(
                    Corrupt(id, "data table: a key's string span runs past the key heap"));
            }
        }

        const AssetResult<AssetHandle<TableSchema>> schemaResult =
            LoadSchema(manager, id, header.SchemaId, async);
        if (!schemaResult)
        {
            return std::unexpected(schemaResult.error());
        }

        const Ref<DataTable> table = DataTable::Create(
            DataTable::Contents{
                .Schema = *schemaResult,
                .KeyKind = static_cast<TableKeyKind>(header.KeyKind),
                .FixedStride = fixedStride,
                .RowStride = header.RowStride,
                .Rows = std::move(rows),
                .RowOffsets = std::move(rowOffsets),
                .Keys = std::move(keys),
                .KeyHeap = std::move(keyHeap),
            },
            types);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(table),
            .Dependencies = {AssetManager::EntryOf(*schemaResult)},
            // The schema is resident by the time Finalize runs, so this is where a table cooked
            // against a since-changed schema layout is caught rather than misread.
            .Finalize = [table, fixedStride, stride = header.RowStride, id]() -> VoidResult
            {
                const TableSchema& schema = table->GetSchema();
                if (schema.IsFixedStride() != fixedStride)
                {
                    return std::unexpected(fmt::format(
                        "data table {}: rows are cooked {} but its schema lays out {} — re-cook "
                        "the pack",
                        id.Value, fixedStride ? "fixed-stride" : "variable-size",
                        schema.IsFixedStride() ? "fixed-stride" : "variable-size"));
                }
                if (fixedStride && schema.GetRowStride() != stride)
                {
                    return std::unexpected(fmt::format(
                        "data table {}: rows are {} bytes but its schema lays out {} — re-cook "
                        "the pack",
                        id.Value, stride, schema.GetRowStride()));
                }
                if (schema.GetKeyKind() != table->GetKeyKind())
                {
                    return std::unexpected(
                        fmt::format("data table {}: its key index and its schema disagree on the "
                                    "key kind — re-cook the pack",
                                    id.Value));
                }
                return {};
            },
        };
    }
}
