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

    AssetResult<Detail::LoadJob>
    TableSchemaLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                            TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                            std::span<const u8> cooked, bool /*async*/) const
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

        const usize columnBytes =
            static_cast<usize>(header.ColumnCount) * sizeof(CookedTableColumn);
        if (cooked.size() < sizeof(CookedTableSchemaHeader) + columnBytes)
        {
            return std::unexpected(Corrupt(id, "table schema: cooked blob truncated"));
        }

        vector<TableColumnDescriptor> columns;
        columns.reserve(header.ColumnCount);
        for (u32 i = 0; i < header.ColumnCount; ++i)
        {
            CookedTableColumn cookedColumn;
            std::memcpy(&cookedColumn,
                        cooked.data() + sizeof(CookedTableSchemaHeader) +
                            i * sizeof(CookedTableColumn),
                        sizeof(CookedTableColumn));

            if (cookedColumn.Kind > static_cast<u32>(CookedTableColumnKind::AssetRef))
            {
                return std::unexpected(
                    Corrupt(id, fmt::format("table schema: column '{}' has unrecognized kind {}",
                                            BridgeName(cookedColumn.Name), cookedColumn.Kind)));
            }

            const auto kind = static_cast<TableColumnKind>(cookedColumn.Kind);
            if (static_cast<usize>(cookedColumn.Offset) + TableCellSize(kind) > header.RowStride)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("table schema: column '{}' cell at offset {} does not fit the "
                                    "{}-byte row",
                                    BridgeName(cookedColumn.Name), cookedColumn.Offset,
                                    header.RowStride)));
            }

            columns.push_back(TableColumnDescriptor{
                .Name = BridgeName(cookedColumn.Name),
                .Kind = kind,
                .Offset = cookedColumn.Offset,
                .ReferencedType = AssetTypeId{cookedColumn.ReferencedType},
            });
        }

        const TableColumnKind keyKind = columns[header.KeyColumn].Kind;
        if (keyKind != TableColumnKind::Int && keyKind != TableColumnKind::String)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("table schema: key column '{}' is neither Int nor String",
                                        columns[header.KeyColumn].Name)));
        }

        return Detail::LoadJob{
            .Resource = Detail::RefAny(
                TableSchema::Create(std::move(columns), header.KeyColumn, header.RowStride)),
        };
    }
}
