// Table cook test: cooks a *.tableschema.json + *.table.json pair through the TableSchemaImporter
// and DataTableImporter and checks the cooked schema layout, the row encoding under both the
// fixed-stride and row-directory paths, and the sorted-unique key index. Also covers each
// validation failure class — an unknown column, a missing column, a type mismatch, a bad
// enumerator, a malformed nested struct, a duplicate key, an unresolvable asset reference, one
// whose target is the wrong asset type, an unkeyable key column, an unregistered column type, and
// a cook carrying no reflected type registry — and that the cook records the schema and every
// referenced asset as dependencies.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include "support/TableTestTypes.h"
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

    // Every column type here encodes to a constant width, so the cook takes the fixed-stride path.
    json FixedSchema()
    {
        json schema;
        schema["columns"] = json::array({
            {{"name", "id"}, {"type", "Veng::i64"}},
            {{"name", "flag"}, {"type", "bool"}},
            {{"name", "weight"}, {"type", "Veng::f32"}},
            {{"name", "offset"}, {"type", "Veng::vec2"}},
            {{"name", "motion"}, {"type", "VengTest::Cadence"}},
            {{"name", "payload"}, {"type", "Veng::AssetHandle<RawAsset>"}},
        });
        schema["key"] = "id";
        return schema;
    }

    json FixedTable()
    {
        json table;
        table["schema"] = FormatHexId(SchemaId);
        table["rows"] = json::array({
            {{"id", 40},
             {"flag", true},
             {"weight", 2.5},
             {"offset", json::array({1.0, 2.0})},
             {"motion", "Burst"},
             {"payload", FormatHexId(BlobId)}},
            {{"id", 10},
             {"flag", false},
             {"weight", -1.25},
             {"offset", json::array({3.0, 4.0})},
             {"motion", "Idle"},
             {"payload", FormatHexId(0)}},
        });
        return table;
    }

    // The fixed columns first, then a string, a nested struct, and an array — so the leading
    // columns keep a constant offset while the table as a whole needs a row directory.
    json MixedSchema()
    {
        json schema = FixedSchema();
        schema["columns"].push_back({{"name", "label"}, {"type", "Veng::string"}});
        schema["columns"].push_back({{"name", "extent"}, {"type", "VengTest::Extent"}});
        schema["columns"].push_back({{"name", "curve"}, {"type", "VengTest::WeightCurve"}});
        return schema;
    }

    json MixedTable()
    {
        json table = FixedTable();
        table["rows"][0]["label"] = "second";
        table["rows"][0]["extent"] = {{"Size", json::array({8.0, 4.0})}, {"Margin", 0.5}};
        table["rows"][0]["curve"] = {{"Samples", json::array({1.0, 2.0, 3.0})}};
        table["rows"][1]["label"] = "first";
        table["rows"][1]["extent"] = {{"Size", json::array({2.0, 1.0})}, {"Margin", 0.25}};
        table["rows"][1]["curve"] = {{"Samples", json::array()}};
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

    // withTypes = false reproduces a cook carrying no reflected type registry, which a table of
    // reflected columns cannot be laid out against.
    Result<CookedPack> CookTablePack(const path& packJson, const bool withTypes = true)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        TypeRegistry types;
        VengTest::RegisterTableTestTypes(types);

        // Unique per call: ctest runs each case as its own process, and a shared fixed name lets
        // concurrent cases cook over and delete each other's archive.
        std::random_device rng;
        const path outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_cooker_table_{:08x}.vengpack", rng());

        CookedPack cooked;
        const VoidResult cookResult = cooker.CookPack(
            packJson, outArchive, {}, withTypes ? &types : nullptr, nullptr, &cooked.Dependencies);
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

    usize DirectoryStart(const CookedDataTableHeader& header)
    {
        return sizeof(CookedDataTableHeader) +
               static_cast<usize>(header.RowCount) * sizeof(CookedTableKey);
    }

    usize RowRegionStart(const CookedDataTableHeader& header)
    {
        return DirectoryStart(header) +
               (header.FixedStride != 0 ? 0 : static_cast<usize>(header.RowCount) * sizeof(u32));
    }

    u32 RowOffset(const vector<u8>& blob, const CookedDataTableHeader& header, const u32 row)
    {
        u32 offset = 0;
        std::memcpy(&offset, blob.data() + DirectoryStart(header) + row * sizeof(u32),
                    sizeof(offset));
        return offset;
    }

    // The bytes of one row within a cooked table blob, under either addressing mode.
    const u8* TableRow(const vector<u8>& blob, const CookedDataTableHeader& header, const u32 row)
    {
        const usize base = RowRegionStart(header);
        if (header.FixedStride != 0)
        {
            return blob.data() + base + static_cast<usize>(row) * header.RowStride;
        }
        return blob.data() + base + RowOffset(blob, header, row);
    }

    std::string_view KeyHeapString(const vector<u8>& blob, const CookedDataTableHeader& header,
                                   const CookedTableStringSpan span)
    {
        const usize heapStart = RowRegionStart(header) + header.RowBytes;
        return std::string_view(
            reinterpret_cast<const char*>(blob.data()) + heapStart + span.Offset, span.Length);
    }
}

TEST_CASE("table cook: an all-fixed-size schema addresses rows arithmetically and omits the "
          "directory")
{
    const path packJson = WriteTablePack("table_fixed", FixedSchema(), FixedTable());

    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    const CookedTableSchemaHeader schemaHeader = SchemaHeader(cooked->Schema);
    CHECK(schemaHeader.Version == CookedTableSchemaVersion);
    CHECK(schemaHeader.ColumnCount == 6);
    CHECK(schemaHeader.KeyColumn == 0);
    CHECK(schemaHeader.FixedStride == 1);
    CHECK(schemaHeader.KeyKind == static_cast<u32>(CookedTableKeyKind::Integer));

    // Cells are packed in declaration order at their encoded widths: i64, bool, f32, vec2, the
    // enum's u32, then the handle's leading id.
    CHECK(SchemaColumn(cooked->Schema, 0).Offset == 0);
    CHECK(SchemaColumn(cooked->Schema, 1).Offset == 8);
    CHECK(SchemaColumn(cooked->Schema, 2).Offset == 9);
    CHECK(SchemaColumn(cooked->Schema, 3).Offset == 13);
    CHECK(SchemaColumn(cooked->Schema, 4).Offset == 21);
    CHECK(SchemaColumn(cooked->Schema, 5).Offset == 25);
    CHECK(schemaHeader.RowStride == 33);

    // A column carries its reflected TypeId, not a bespoke kind enumerator.
    const CookedTableColumn payload = SchemaColumn(cooked->Schema, 5);
    CHECK(string(payload.Name) == "payload");
    CHECK(payload.Type == TypeIdOf<AssetHandle<RawAsset>>());

    const CookedDataTableHeader header = TableHeader(cooked->Table);
    CHECK(header.Version == CookedDataTableVersion);
    CHECK(header.RowCount == 2);
    CHECK(header.FixedStride == 1);
    CHECK(header.RowStride == schemaHeader.RowStride);
    CHECK(header.RowBytes == 2 * schemaHeader.RowStride);
    CHECK(header.SchemaId == SchemaId);

    // The key index is sorted ascending and unique, whatever order the rows were authored in.
    CHECK(TableKey(cooked->Table, 0).IntKey == 10);
    CHECK(TableKey(cooked->Table, 1).IntKey == 40);
    CHECK(TableKey(cooked->Table, 0).RowIndex == 1);
    CHECK(TableKey(cooked->Table, 1).RowIndex == 0);

    const u8* const row = TableRow(cooked->Table, header, 0);
    i64 id = 0;
    std::memcpy(&id, row + 0, sizeof(id));
    CHECK(id == 40);

    f32 weight = 0.0f;
    std::memcpy(&weight, row + 9, sizeof(weight));
    CHECK(weight == doctest::Approx(2.5f));

    u32 motion = 0;
    std::memcpy(&motion, row + 21, sizeof(motion));
    CHECK(motion == static_cast<u32>(VengTest::Cadence::Burst));

    u64 reference = 0;
    std::memcpy(&reference, row + 25, sizeof(reference));
    CHECK(reference == BlobId);

    // --- Dependencies: the schema source and the referenced asset, recorded through Resolve ---
    const auto names = [&cooked](const string& suffix)
    {
        return std::ranges::any_of(cooked->Dependencies, [&suffix](const path& dependency)
                                   { return dependency.string().ends_with(suffix); });
    };
    CHECK(names("table_fixed.tableschema.json"));
    CHECK(names("table_fixed.table.json"));
    CHECK(names("table_fixed.bin"));
}

TEST_CASE("table cook: a string, struct, or array column makes rows variable-size and adds a "
          "directory")
{
    const path packJson = WriteTablePack("table_mixed", MixedSchema(), MixedTable());

    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    const CookedTableSchemaHeader schemaHeader = SchemaHeader(cooked->Schema);
    CHECK(schemaHeader.ColumnCount == 9);
    CHECK(schemaHeader.FixedStride == 0);

    // Leading fixed columns keep a constant offset; the first variable column and everything
    // after it does not.
    CHECK(SchemaColumn(cooked->Schema, 0).Offset == 0);
    CHECK(SchemaColumn(cooked->Schema, 5).Offset == 25);
    CHECK(SchemaColumn(cooked->Schema, 6).Offset == CookedTableColumnOffsetUnresolved);
    CHECK(SchemaColumn(cooked->Schema, 7).Offset == CookedTableColumnOffsetUnresolved);
    CHECK(SchemaColumn(cooked->Schema, 8).Offset == CookedTableColumnOffsetUnresolved);

    const CookedDataTableHeader header = TableHeader(cooked->Table);
    CHECK(header.FixedStride == 0);
    REQUIRE(header.RowCount == 2);

    // The directory starts at zero, and the two rows differ in size because their arrays do.
    CHECK(RowOffset(cooked->Table, header, 0) == 0);
    const u32 firstRowBytes = RowOffset(cooked->Table, header, 1);
    const u32 secondRowBytes = header.RowBytes - firstRowBytes;
    CHECK(firstRowBytes > 0);
    CHECK(firstRowBytes > secondRowBytes);

    // The leading fixed cells still decode arithmetically in a variable-size row.
    const u8* const row = TableRow(cooked->Table, header, 0);
    i64 id = 0;
    std::memcpy(&id, row + 0, sizeof(id));
    CHECK(id == 40);
}

TEST_CASE("table cook: a String key column sorts the key index lexicographically")
{
    json schema;
    schema["columns"] = json::array(
        {{{"name", "name"}, {"type", "Veng::string"}}, {{"name", "count"}, {"type", "Veng::i64"}}});
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
    CHECK(header.KeyKind == static_cast<u32>(CookedTableKeyKind::String));
    REQUIRE(header.RowCount == 3);

    CHECK(KeyHeapString(cooked->Table, header, TableKey(cooked->Table, 0).StringKey) == "alpha");
    CHECK(KeyHeapString(cooked->Table, header, TableKey(cooked->Table, 1).StringKey) == "beta");
    CHECK(KeyHeapString(cooked->Table, header, TableKey(cooked->Table, 2).StringKey) == "gamma");
    CHECK(TableKey(cooked->Table, 0).RowIndex == 1);
}

TEST_CASE("table cook: without a reflected type registry the cook fails loudly")
{
    const path packJson = WriteTablePack("table_no_registry", FixedSchema(), FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson, false);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("needs the reflected type registry") != string::npos);
}

TEST_CASE("table cook: an unknown column in a row is a located error")
{
    json table = FixedTable();
    table["rows"][0]["bogus"] = 1;

    const path packJson = WriteTablePack("table_unknown_column", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("no column named 'bogus'") != string::npos);
}

TEST_CASE("table cook: a missing column in a row is a located error")
{
    json table = FixedTable();
    table["rows"][0].erase("weight");

    const path packJson = WriteTablePack("table_missing_column", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("column 'weight': missing") != string::npos);
}

TEST_CASE("table cook: a cell of the wrong type is a located error naming the column")
{
    json table = FixedTable();
    table["rows"][0]["flag"] = "yes";

    const path packJson = WriteTablePack("table_type_mismatch", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("row 0: flag") != string::npos);
    CHECK(cooked.error().find("expected a number or boolean") != string::npos);
}

TEST_CASE("table cook: an unknown enumerator in an enum column is a located error")
{
    json table = FixedTable();
    table["rows"][0]["motion"] = "Frantic";

    const path packJson = WriteTablePack("table_bad_enum", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("unknown enumerator 'Frantic'") != string::npos);
}

TEST_CASE("table cook: a malformed nested-struct cell is located down to the inner field")
{
    json table = MixedTable();
    table["rows"][0]["extent"]["Margin"] = "wide";

    const path packJson = WriteTablePack("table_bad_struct", MixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("extent.Margin") != string::npos);
}

TEST_CASE("table cook: a duplicate key is a located error")
{
    json table = FixedTable();
    table["rows"][1]["id"] = 40;

    const path packJson = WriteTablePack("table_duplicate_key", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("appears in more than one row") != string::npos);
}

TEST_CASE("table cook: an asset reference naming an undeclared asset is a located error")
{
    json table = FixedTable();
    table["rows"][0]["payload"] = FormatHexId(0xDEADBEEFULL);

    const path packJson = WriteTablePack("table_unresolved_ref", FixedSchema(), table);
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("is not declared in this pack") != string::npos);
}

TEST_CASE("table cook: an asset reference of the wrong asset type is a located error")
{
    json schema = FixedSchema();
    schema["columns"][5]["type"] = "Veng::AssetHandle<Texture>";

    const path packJson = WriteTablePack("table_ref_type", schema, FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("but the column expects type Texture") != string::npos);
}

TEST_CASE("table schema cook: a key column with no total order is a located error")
{
    json schema = FixedSchema();
    schema["key"] = "weight";

    const path packJson = WriteTablePack("table_bad_key", schema, FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("must be an integer or a string") != string::npos);
}

TEST_CASE("table schema cook: an unregistered column type is a located error")
{
    json schema = FixedSchema();
    schema["columns"][1]["type"] = "Veng::Colour";

    const path packJson = WriteTablePack("table_bad_type", schema, FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("no reflected type named 'Veng::Colour' is registered") !=
          string::npos);
}

TEST_CASE("table schema cook: a type name must be fully qualified to match")
{
    json schema = FixedSchema();
    schema["columns"][2]["type"] = "f32";

    const path packJson = WriteTablePack("table_unqualified", schema, FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("no reflected type named 'f32' is registered") != string::npos);
}

TEST_CASE("table schema cook: a duplicate column name is a located error")
{
    json schema = FixedSchema();
    schema["columns"][2]["name"] = "flag";

    const path packJson = WriteTablePack("table_dup_column", schema, FixedTable());
    const Result<CookedPack> cooked = CookTablePack(packJson);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("is declared more than once") != string::npos);
}
