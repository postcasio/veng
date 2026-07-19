#include "TableSchemaLoader.h"

#include <cstring>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Cooked names are fixed-size, nul-terminated char arrays (CookedBlobs.h).
        template <usize N>
        string BridgeName(const char (&name)[N])
        {
            return string(name, strnlen(name, N));
        }
    }

    AssetResult<Detail::LoadJob> TableSchemaLoader::Load(AssetManager& /*manager*/,
                                                         Renderer::Context& /*context*/,
                                                         TaskSystem& /*tasks*/, TypeRegistry& types,
                                                         AssetId id, std::span<const u8> cooked,
                                                         bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedTableSchemaHeader))
        {
            return std::unexpected(Corrupt(id, "table schema: cooked blob smaller than header"));
        }

        CookedTableSchemaHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedTableSchemaVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("table schema: blob version {} does not match version {} — "
                                        "re-cook the pack",
                                        header.Version, CookedTableSchemaVersion)));
        }
        if (header.ColumnCount == 0)
        {
            return std::unexpected(Corrupt(id, "table schema: blob declares no columns"));
        }
        if (header.KeyColumn >= header.ColumnCount)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("table schema: key column {} is outside the {} declared columns",
                                header.KeyColumn, header.ColumnCount)));
        }
        if (header.KeyKind != static_cast<u32>(CookedTableKeyKind::Integer) &&
            header.KeyKind != static_cast<u32>(CookedTableKeyKind::String))
        {
            return std::unexpected(
                Corrupt(id, fmt::format("table schema: key kind {} is neither Integer nor String",
                                        header.KeyKind)));
        }

        const usize columnBytes =
            static_cast<usize>(header.ColumnCount) * sizeof(CookedTableColumn);
        if (cooked.size() < sizeof(CookedTableSchemaHeader) + columnBytes)
        {
            return std::unexpected(Corrupt(id, "table schema: cooked blob truncated"));
        }

        // The layout is re-derived from the registry rather than trusted from the blob, then
        // cross-checked against it: a type whose reflected width changed since the cook would
        // otherwise silently misaddress every cell.
        vector<TableColumnDescriptor> columns;
        columns.reserve(header.ColumnCount);
        u32 cursor = 0;
        bool arithmetic = true;
        bool allFixed = true;

        for (u32 i = 0; i < header.ColumnCount; ++i)
        {
            CookedTableColumn cookedColumn;
            std::memcpy(&cookedColumn,
                        cooked.data() + sizeof(CookedTableSchemaHeader) +
                            i * sizeof(CookedTableColumn),
                        sizeof(CookedTableColumn));

            const string name = BridgeName(cookedColumn.Name);
            const TypeId type = cookedColumn.Type;
            if (type == InvalidTypeId || !types.IsRegistered(type))
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("table schema: column '{}' names type {:#018x}, which this "
                                    "host does not have registered",
                                    name, type)));
            }

            const TypeInfo& info = types.Info(type);
            const bool fixed = TableCellIsFixedSize(info.Class);
            allFixed = allFixed && fixed;

            u32 offset = CookedTableColumnOffsetUnresolved;
            if (arithmetic && fixed)
            {
                offset = cursor;
                cursor += TableCellEncodedSize(info.Class, info);
            }
            else
            {
                arithmetic = false;
            }

            if (offset != cookedColumn.Offset)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("table schema: column '{}' is cooked at offset {} but type "
                                    "'{}' lays out at {} — re-cook the pack",
                                    name, cookedColumn.Offset, info.QualifiedName, offset)));
            }

            columns.push_back(TableColumnDescriptor{
                .Name = name, .Type = type, .Class = info.Class, .Offset = offset});
        }

        if ((header.FixedStride != 0) != allFixed)
        {
            return std::unexpected(
                Corrupt(id, "table schema: the blob's fixed-stride flag disagrees with its column "
                            "types — re-cook the pack"));
        }
        if (allFixed && header.RowStride != cursor)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("table schema: rows are cooked at {} bytes but these columns lay "
                                "out at {} — re-cook the pack",
                                header.RowStride, cursor)));
        }

        const TableColumnDescriptor& key = columns[header.KeyColumn];
        const optional<TableKeyKind> keyKind = TableKeyKindForType(key.Type, key.Class);
        if (!keyKind)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("table schema: key column '{}' has type '{}', which has no total "
                                "order a key index can be built on",
                                key.Name, types.Info(key.Type).QualifiedName)));
        }
        if (static_cast<u32>(*keyKind) != header.KeyKind)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("table schema: key column '{}' is cooked under key kind {} "
                                        "but its type orders as {}",
                                        key.Name, header.KeyKind, static_cast<u32>(*keyKind))));
        }

        const u32 rowStride = allFixed ? cursor : 0;
        return Detail::LoadJob{
            .Resource = Detail::RefAny(TableSchema::Create(std::move(columns), header.KeyColumn,
                                                           *keyKind, allFixed, rowStride)),
        };
    }
}
