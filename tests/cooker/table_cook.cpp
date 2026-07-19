// Table cook test: cooks a *.tableschema.json + *.table.json pair through the TableSchemaImporter
// and DataTableImporter and checks the cooked schema layout, the fixed-stride rows, the string
// heap, and the sorted-unique key index. Also covers each validation failure class — an unknown
// column, a missing column, a kind mismatch, a duplicate key, an unresolvable AssetRef and an
// AssetRef whose target is the wrong asset type — and that the cook records the schema and every
// AssetRef target as dependencies. Tables reference only ids, so the cook needs no --module.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr u64 SchemaId = 0x7AB1E5C0DE000001ULL;
    constexpr u64 TableId = 0x7AB1E5C0DE000002ULL;
    constexpr u64 BlobId = 0x7AB1E5C0DE000003ULL;

    // A schema with one column of every kind, keyed on an Int column.
    json SampleSchema()
    {
        json schema;
        schema["columns"] = json::array({
            {{"name", "id"}, {"kind", "Int"}},
            {{"name", "flag"}, {"kind", "Bool"}},
            {{"name", "weight"}, {"kind", "Float"}},
            {{"name", "offset"}, {"kind", "Vec2"}},
            {{"name", "colour"}, {"kind", "Vec4"}},
            {{"name", "label"}, {"kind", "String"}},
            {{"name", "payload"}, {"kind", "AssetRef"}, {"assetType", "Raw"}},
        });
        schema["key"] = "id";
        return schema;
    }

    // Two rows authored out of key order, so a passing sort assertion means something.
    json SampleTable()
    {
        json table;
        table["schema"] = FormatHexId(SchemaId);
        table["rows"] = json::array({
            {{"id", 40},
             {"flag", true},
             {"weight", 2.5},
             {"offset", json::array({1.0, 2.0})},
             {"colour", json::array({0.1, 0.2, 0.3, 1.0})},
             {"label", "second"},
             {"payload", FormatHexId(BlobId)}},
            {{"id", 10},
             {"flag", false},
             {"weight", -1.25},
             {"offset", json::array({3.0, 4.0})},
             {"colour", json::array({0.4, 0.5, 0.6, 1.0})},
             {"label", "first"},
             {"payload", FormatHexId(0)}},
        });
        return table;
    }

    // Writes the schema, table, and referenced raw blob plus a three-entry pack, returning the pack.
    path WriteTablePack(const string& name, const json& schema, const json& table)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path schemaPath = dir / (name + ".tableschema.json");
        const path tablePath = dir / (name + ".table.json");
        const path blobPath = dir / (name + ".bin");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(schemaPath) << schema.dump();
        std::ofstream(tablePath) << table.dump();
        std::ofstream(blobPath, std::ios::binary) << "payload";

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({
            {{"id", FormatHexId(SchemaId)},
             {"type", "TableSchema"},
             {"source", schemaPath.filename().string()}},
            {{"id", FormatHexId(TableId)},
             {"type", "DataTable"},
             {"source", tablePath.filename().string()}},
            {{"id", FormatHexId(BlobId)},
             {"type", "Raw"},
             {"source", blobPath.filename().string()}},
        });
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // One cooked pack's blobs, keyed by AssetId, plus the dependency list the cook recorded.
    struct CookedPack
    {
        vector<u8> Schema;
        vector<u8> Table;
        vector<path> Dependencies;
    };

    Result<CookedPack> CookTablePack(const path& packJson)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        // Unique per call: ctest runs each case as its own process, and a shared fixed name lets
        // concurrent cases cook over and delete each other's archive.
        std::random_device rng;
        const path outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_cooker_table_{:08x}.vengpack", rng());

        CookedPack cooked;
        const VoidResult cookResult =
            cooker.CookPack(packJson, outArchive, {}, nullptr, nullptr, &cooked.Dependencies);
        if (!cookResult.has_value())
        {
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(reader.error());
        }

        const optional<ArchiveEntry> schemaEntry = reader->Find(AssetId{SchemaId});
        const optional<ArchiveEntry> tableEntry = reader->Find(AssetId{TableId});
        if (!schemaEntry.has_value() || !tableEntry.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(string("table entries missing from archive"));
        }
        cooked.Schema.assign(schemaEntry->Blob.begin(), schemaEntry->Blob.end());
        cooked.Table.assign(tableEntry->Blob.begin(), tableEntry->Blob.end());

        std::filesystem::remove(outArchive);
        return cooked;
    }

    CookedTableSchemaHeader SchemaHeader(const vector<u8>& blob)
    {
        CookedTableSchemaHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        return header;
    }

    CookedTableColumn SchemaColumn(const vector<u8>& blob, const u32 index)
    {
        CookedTableColumn column{};
        std::memcpy(&column,
                    blob.data() + sizeof(CookedTableSchemaHeader) +
                        index * sizeof(CookedTableColumn),
                    sizeof(column));
        return column;
    }

    CookedDataTableHeader TableHeader(const vector<u8>& blob)
    {
        CookedDataTableHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        return header;
    }

    CookedTableKey TableKey(const vector<u8>& blob, const u32 index)
    {
        CookedTableKey key{};
        std::memcpy(&key, blob.data() + sizeof(CookedDataTableHeader) + index * sizeof(key),
                    sizeof(key));
        return key;
    }

    // The bytes of one row record within a cooked table blob.
    const u8* TableRow(const vector<u8>& blob, const CookedDataTableHeader& header, const u32 row)
    {
        return blob.data() + sizeof(CookedDataTableHeader) +
               static_cast<usize>(header.RowCount) * sizeof(CookedTableKey) +
               static_cast<usize>(row) * header.RowStride;
    }

    std::string_view TableString(const vector<u8>& blob, const CookedDataTableHeader& header,
                                 const CookedTableStringSpan span)
    {
        const usize heapStart = sizeof(CookedDataTableHeader) +
                                static_cast<usize>(header.RowCount) * sizeof(CookedTableKey) +
                                static_cast<usize>(header.RowCount) * header.RowStride;
        return std::string_view(
            reinterpret_cast<const char*>(blob.data()) + heapStart + span.Offset, span.Length);
    }
}

TEST_CASE("table cook: happy path — schema layout, rows, string heap, and sorted key index")
{
    const path packJson = WriteTablePack("table_happy", SampleSchema(), SampleTable());

    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    // --- Schema blob: one column per declaration, each cell on its own alignment ---
    const CookedTableSchemaHeader schemaHeader = SchemaHeader(cooked->Schema);
    CHECK(schemaHeader.Version == CookedTableSchemaVersion);
    CHECK(schemaHeader.ColumnCount == 7);
    CHECK(schemaHeader.KeyColumn == 0);
    CHECK(schemaHeader.RowStride % 8 == 0);

    for (u32 i = 0; i < schemaHeader.ColumnCount; ++i)
    {
        const CookedTableColumn column = SchemaColumn(cooked->Schema, i);
        const auto kind = static_cast<TableColumnKind>(column.Kind);
        CHECK(column.Offset % TableCellAlignment(kind) == 0);
        CHECK(column.Offset + TableCellSize(kind) <= schemaHeader.RowStride);
    }

    // The AssetRef column carries the asset type it constrains its cells to.
    const CookedTableColumn payload = SchemaColumn(cooked->Schema, 6);
    CHECK(string(payload.Name) == "payload");
    CHECK(payload.Kind == static_cast<u32>(CookedTableColumnKind::AssetRef));
    CHECK(payload.ReferencedType == AssetTypes::Raw.Value);

    // --- Table blob ---
    const CookedDataTableHeader header = TableHeader(cooked->Table);
    CHECK(header.Version == CookedDataTableVersion);
    CHECK(header.RowCount == 2);
    CHECK(header.RowStride == schemaHeader.RowStride);
    CHECK(header.SchemaId == SchemaId);
    CHECK(header.KeyKind == static_cast<u32>(CookedTableColumnKind::Int));

    // The key index is sorted ascending and unique, whatever order the rows were authored in.
    const CookedTableKey first = TableKey(cooked->Table, 0);
    const CookedTableKey second = TableKey(cooked->Table, 1);
    CHECK(first.IntKey == 10);
    CHECK(second.IntKey == 40);
    CHECK(first.IntKey < second.IntKey);
    CHECK(first.RowIndex == 1);
    CHECK(second.RowIndex == 0);

    // Row 0 is the authored first row (id 40): its cells decode at the schema's offsets.
    const u8* const row = TableRow(cooked->Table, header, 0);
    i64 id = 0;
    std::memcpy(&id, row + SchemaColumn(cooked->Schema, 0).Offset, sizeof(id));
    CHECK(id == 40);

    u32 flag = 0;
    std::memcpy(&flag, row + SchemaColumn(cooked->Schema, 1).Offset, sizeof(flag));
    CHECK(flag == 1);

    f32 weight = 0.0f;
    std::memcpy(&weight, row + SchemaColumn(cooked->Schema, 2).Offset, sizeof(weight));
    CHECK(weight == doctest::Approx(2.5f));

    CookedTableStringSpan label{};
    std::memcpy(&label, row + SchemaColumn(cooked->Schema, 5).Offset, sizeof(label));
    CHECK(TableString(cooked->Table, header, label) == "second");

    u64 reference = 0;
    std::memcpy(&reference, row + payload.Offset, sizeof(reference));
    CHECK(reference == BlobId);

    // The unset reference in the other row stays the invalid id.
    const u8* const otherRow = TableRow(cooked->Table, header, 1);
    std::memcpy(&reference, otherRow + payload.Offset, sizeof(reference));
    CHECK(reference == 0);

    // --- Dependencies: the schema source and the AssetRef target, recorded through Resolve ---
    const auto names = [&cooked](const string& suffix)
    {
        return std::ranges::any_of(cooked->Dependencies, [&suffix](const path& dependency)
                                   { return dependency.string().ends_with(suffix); });
    };
    CHECK(names("table_happy.tableschema.json"));
    CHECK(names("table_happy.table.json"));
    CHECK(names("table_happy.bin"));
}

TEST_CASE("table cook: a String key column sorts the key index lexicographically")
{
    json schema;
    schema["columns"] =
        json::array({{{"name", "name"}, {"kind", "String"}}, {{"name", "count"}, {"kind", "Int"}}});
    schema["key"] = "name";

    json table;
    table["schema"] = FormatHexId(SchemaId);
    table["rows"] = json::array({{{"name", "gamma"}, {"count", 3}},
                                 {{"name", "alpha"}, {"count", 1}},
                                 {{"name", "beta"}, {"count", 2}}});

    const path packJson = WriteTablePack("table_stringkey", schema, table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    const CookedDataTableHeader header = TableHeader(cooked->Table);
    CHECK(header.KeyKind == static_cast<u32>(CookedTableColumnKind::String));
    REQUIRE(header.RowCount == 3);

    CHECK(TableString(cooked->Table, header, TableKey(cooked->Table, 0).StringKey) == "alpha");
    CHECK(TableString(cooked->Table, header, TableKey(cooked->Table, 1).StringKey) == "beta");
    CHECK(TableString(cooked->Table, header, TableKey(cooked->Table, 2).StringKey) == "gamma");
    CHECK(TableKey(cooked->Table, 0).RowIndex == 1);
}

TEST_CASE("table cook: an unknown column in a row is a located error")
{
    json table = SampleTable();
    table["rows"][0]["bogus"] = 1;

    const path packJson = WriteTablePack("table_unknown_column", SampleSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("no column named 'bogus'") != string::npos);
}

TEST_CASE("table cook: a missing column in a row is a located error")
{
    json table = SampleTable();
    table["rows"][0].erase("weight");

    const path packJson = WriteTablePack("table_missing_column", SampleSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("column 'weight': missing") != string::npos);
}

TEST_CASE("table cook: a cell of the wrong kind is a located error")
{
    json table = SampleTable();
    table["rows"][0]["flag"] = "yes";

    const path packJson = WriteTablePack("table_kind_mismatch", SampleSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("expected a boolean") != string::npos);
}

TEST_CASE("table cook: a duplicate key is a located error")
{
    json table = SampleTable();
    table["rows"][1]["id"] = 40;

    const path packJson = WriteTablePack("table_duplicate_key", SampleSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("appears in more than one row") != string::npos);
}

TEST_CASE("table cook: an AssetRef naming an undeclared asset is a located error")
{
    json table = SampleTable();
    table["rows"][0]["payload"] = FormatHexId(0xDEADBEEFULL);

    const path packJson = WriteTablePack("table_unresolved_ref", SampleSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("is not declared in this pack") != string::npos);
}

TEST_CASE("table cook: an AssetRef of the wrong asset type is a located error")
{
    json schema = SampleSchema();
    schema["columns"][6]["assetType"] = "Texture";

    const path packJson = WriteTablePack("table_ref_type", schema, SampleTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("but the column references Texture") != string::npos);
}

TEST_CASE("table schema cook: a key column of an unkeyable kind is a located error")
{
    json schema = SampleSchema();
    schema["key"] = "weight";

    const path packJson = WriteTablePack("table_bad_key", schema, SampleTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("must be kind 'Int' or 'String'") != string::npos);
}

TEST_CASE("table schema cook: an unknown column kind is a located error")
{
    json schema = SampleSchema();
    schema["columns"][1]["kind"] = "Colour";

    const path packJson = WriteTablePack("table_bad_kind", schema, SampleTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("unknown kind 'Colour'") != string::npos);
}

TEST_CASE("table schema cook: a duplicate column name is a located error")
{
    json schema = SampleSchema();
    schema["columns"][2]["name"] = "flag";

    const path packJson = WriteTablePack("table_dup_column", schema, SampleTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("is declared more than once") != string::npos);
}
