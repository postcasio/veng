// DataTable runtime cases: the cooked schema + table blobs load through the ordinary
// AssetManager path (no GPU — Context is default-constructed and neither loader touches it),
// FindRow hits and misses on both key kinds, typed accessors decode every column kind, an
// AssetRef cell yields a bare AssetId, and a truncated blob is rejected as Corrupt rather than
// asserted on. The blobs are assembled by hand here so the runtime side is tested independently
// of the cooker.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    constexpr AssetId SchemaId{0x5AB1E0000000001ULL};
    constexpr AssetId TableId{0x5AB1E0000000002ULL};
    constexpr AssetId ReferencedId{0x5AB1E0000000003ULL};

    template <class T>
    void Append(vector<u8>& out, const T& value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }

    struct Column
    {
        string Name;
        TableColumnKind Kind = TableColumnKind::Int;
        u32 Offset = 0;
    };

    // Columns matching the layout rule the schema cook applies: natural alignment, stride to 8.
    vector<Column> Columns()
    {
        return {
            {.Name = "id", .Kind = TableColumnKind::Int, .Offset = 0},
            {.Name = "flag", .Kind = TableColumnKind::Bool, .Offset = 8},
            {.Name = "weight", .Kind = TableColumnKind::Float, .Offset = 12},
            {.Name = "offset", .Kind = TableColumnKind::Vec2, .Offset = 16},
            {.Name = "label", .Kind = TableColumnKind::String, .Offset = 24},
            {.Name = "payload", .Kind = TableColumnKind::AssetRef, .Offset = 32},
        };
    }

    constexpr u32 RowStride = 40;

    vector<u8> BuildSchemaBlob(const u32 keyColumn = 0)
    {
        const vector<Column> columns = Columns();

        CookedTableSchemaHeader header{};
        header.Version = CookedTableSchemaVersion;
        header.ColumnCount = static_cast<u32>(columns.size());
        header.KeyColumn = keyColumn;
        header.RowStride = RowStride;

        vector<u8> blob;
        Append(blob, header);
        for (const Column& column : columns)
        {
            CookedTableColumn cooked{};
            std::memcpy(cooked.Name, column.Name.data(), column.Name.size());
            cooked.Kind = static_cast<u32>(column.Kind);
            cooked.Offset = column.Offset;
            cooked.ReferencedType =
                column.Kind == TableColumnKind::AssetRef ? AssetTypes::Raw.Value : 0;
            Append(blob, cooked);
        }
        return blob;
    }

    // Two rows keyed 10 and 40, with the key index sorted ascending as the cook writes it.
    vector<u8> BuildTableBlob()
    {
        const string heap = "firstsecond";

        vector<u8> rows(2 * RowStride, 0);
        const auto writeRow = [&rows](const u32 row, const i64 id, const u32 flag, const f32 weight,
                                      const f32 x, const f32 y, const u32 stringOffset,
                                      const u32 stringLength, const u64 reference)
        {
            u8* const record = rows.data() + static_cast<usize>(row) * RowStride;
            std::memcpy(record + 0, &id, sizeof(id));
            std::memcpy(record + 8, &flag, sizeof(flag));
            std::memcpy(record + 12, &weight, sizeof(weight));
            std::memcpy(record + 16, &x, sizeof(x));
            std::memcpy(record + 20, &y, sizeof(y));
            const CookedTableStringSpan span{.Offset = stringOffset, .Length = stringLength};
            std::memcpy(record + 24, &span, sizeof(span));
            std::memcpy(record + 32, &reference, sizeof(reference));
        };
        writeRow(0, 10, 1, 2.5f, 1.0f, 2.0f, 0, 5, ReferencedId.Value);
        writeRow(1, 40, 0, -1.25f, 3.0f, 4.0f, 5, 6, 0);

        CookedDataTableHeader header{};
        header.Version = CookedDataTableVersion;
        header.RowCount = 2;
        header.RowStride = RowStride;
        header.StringHeapBytes = static_cast<u32>(heap.size());
        header.SchemaId = SchemaId.Value;
        header.KeyKind = static_cast<u32>(CookedTableColumnKind::Int);

        vector<u8> blob;
        Append(blob, header);
        Append(blob, CookedTableKey{.IntKey = 10, .StringKey = {}, .RowIndex = 0, .Pad = 0});
        Append(blob, CookedTableKey{.IntKey = 40, .StringKey = {}, .RowIndex = 1, .Pad = 0});
        blob.insert(blob.end(), rows.begin(), rows.end());
        blob.insert(blob.end(), heap.begin(), heap.end());
        return blob;
    }

    // A string-keyed variant of the same rows: the key column is "label" and the index is sorted
    // lexicographically over the heap.
    vector<u8> BuildStringKeyedTableBlob()
    {
        vector<u8> blob = BuildTableBlob();
        CookedDataTableHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        header.KeyKind = static_cast<u32>(CookedTableColumnKind::String);
        std::memcpy(blob.data(), &header, sizeof(header));

        // "first" (row 0) sorts before "second" (row 1).
        const CookedTableKey first{
            .IntKey = 0, .StringKey = {.Offset = 0, .Length = 5}, .RowIndex = 0, .Pad = 0};
        const CookedTableKey second{
            .IntKey = 0, .StringKey = {.Offset = 5, .Length = 6}, .RowIndex = 1, .Pad = 0};
        std::memcpy(blob.data() + sizeof(header), &first, sizeof(first));
        std::memcpy(blob.data() + sizeof(header) + sizeof(first), &second, sizeof(second));
        return blob;
    }

    path WriteFixtureArchive(const string& name, const vector<u8>& schemaBlob,
                             const vector<u8>& tableBlob)
    {
        ArchiveWriter writer;
        writer.Add(SchemaId, AssetTypes::TableSchema, schemaBlob);
        writer.Add(TableId, AssetTypes::DataTable, tableBlob);
        writer.Add(ReferencedId, AssetTypes::Raw, vector<u8>{7});

        const path archivePath = Veng::TestSupport::TempDir() / (name + ".vengpack");
        const VoidResult written = writer.Write(archivePath);
        REQUIRE(written.has_value());
        return archivePath;
    }
}

TEST_CASE("DataTable: loads with its schema and decodes every column kind")
{
    const path archivePath =
        WriteFixtureArchive("veng_data_table_unit", BuildSchemaBlob(), BuildTableBlob());

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = manager.LoadSync<DataTable>(TableId);
    REQUIRE(loaded.has_value());

    const AssetHandle<DataTable>& table = *loaded;
    CHECK(table->GetRowCount() == 2);
    CHECK(table->GetRowStride() == RowStride);
    CHECK(table->GetKeyKind() == TableColumnKind::Int);

    // The schema streamed in as an ordinary dependency, so the table can resolve columns by name.
    const TableSchema& schema = table->GetSchema();
    CHECK(schema.GetColumns().size() == 6);
    CHECK(schema.GetKeyColumn().Name == "id");
    CHECK(schema.FindColumn("weight") != nullptr);
    CHECK(schema.FindColumn("absent") == nullptr);

    const TableColumn<i64> id = table->GetColumn<i64>("id");
    const TableColumn<bool> flag = table->GetColumn<bool>("flag");
    const TableColumn<f32> weight = table->GetColumn<f32>("weight");
    const TableColumn<vec2> offset = table->GetColumn<vec2>("offset");
    const TableColumn<std::string_view> label = table->GetColumn<std::string_view>("label");
    const TableColumn<AssetId> payload = table->GetColumn<AssetId>("payload");

    CHECK(id[0] == 10);
    CHECK(flag[0]);
    CHECK(weight[0] == doctest::Approx(2.5f));
    CHECK(offset[0].x == doctest::Approx(1.0f));
    CHECK(offset[0].y == doctest::Approx(2.0f));
    CHECK(label[0] == "first");
    CHECK(payload[0] == ReferencedId);

    CHECK(id[1] == 40);
    CHECK_FALSE(flag[1]);
    CHECK(label[1] == "second");
    // The unset AssetRef stays invalid: the table holds an id, never a handle, and loads nothing.
    CHECK_FALSE(payload[1].IsValid());

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: FindRow hits and misses on an integer key")
{
    const path archivePath =
        WriteFixtureArchive("veng_data_table_findrow", BuildSchemaBlob(), BuildTableBlob());

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = manager.LoadSync<DataTable>(TableId);
    REQUIRE(loaded.has_value());
    const AssetHandle<DataTable>& table = *loaded;

    REQUIRE(table->FindRow(static_cast<i64>(10)).has_value());
    CHECK(*table->FindRow(static_cast<i64>(10)) == 0);
    REQUIRE(table->FindRow(static_cast<i64>(40)).has_value());
    CHECK(*table->FindRow(static_cast<i64>(40)) == 1);

    CHECK_FALSE(table->FindRow(static_cast<i64>(0)).has_value());
    CHECK_FALSE(table->FindRow(static_cast<i64>(25)).has_value());
    CHECK_FALSE(table->FindRow(static_cast<i64>(99)).has_value());

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: FindRow hits and misses on a string key")
{
    const path archivePath = WriteFixtureArchive("veng_data_table_stringkey", BuildSchemaBlob(4),
                                                 BuildStringKeyedTableBlob());

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = manager.LoadSync<DataTable>(TableId);
    REQUIRE(loaded.has_value());
    const AssetHandle<DataTable>& table = *loaded;

    CHECK(table->GetKeyKind() == TableColumnKind::String);
    REQUIRE(table->FindRow(std::string_view("first")).has_value());
    CHECK(*table->FindRow(std::string_view("first")) == 0);
    REQUIRE(table->FindRow(std::string_view("second")).has_value());
    CHECK(*table->FindRow(std::string_view("second")) == 1);
    CHECK_FALSE(table->FindRow(std::string_view("third")).has_value());
    CHECK_FALSE(table->FindRow(std::string_view("")).has_value());

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: a truncated blob is Corrupt, not a crash")
{
    vector<u8> truncated = BuildTableBlob();
    truncated.resize(sizeof(CookedDataTableHeader) + 4);

    const path archivePath =
        WriteFixtureArchive("veng_data_table_truncated", BuildSchemaBlob(), truncated);

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = manager.LoadSync<DataTable>(TableId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);

    std::filesystem::remove(archivePath);
}

TEST_CASE("TableSchema: a version-mismatched schema blob is Corrupt")
{
    vector<u8> schemaBlob = BuildSchemaBlob();
    CookedTableSchemaHeader header{};
    std::memcpy(&header, schemaBlob.data(), sizeof(header));
    header.Version = CookedTableSchemaVersion + 1;
    std::memcpy(schemaBlob.data(), &header, sizeof(header));

    const path archivePath =
        WriteFixtureArchive("veng_data_table_badversion", schemaBlob, BuildTableBlob());

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<TableSchema>> loaded = manager.LoadSync<TableSchema>(SchemaId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);

    std::filesystem::remove(archivePath);
}

TEST_CASE("Column kinds map onto the FieldClass vocabulary the inspector draws")
{
    CHECK(FieldClassForColumnKind(TableColumnKind::Bool) == FieldClass::Scalar);
    CHECK(FieldClassForColumnKind(TableColumnKind::Int) == FieldClass::Scalar);
    CHECK(FieldClassForColumnKind(TableColumnKind::Float) == FieldClass::Scalar);
    CHECK(FieldClassForColumnKind(TableColumnKind::Vec2) == FieldClass::Vector);
    CHECK(FieldClassForColumnKind(TableColumnKind::Vec3) == FieldClass::Vector);
    CHECK(FieldClassForColumnKind(TableColumnKind::Vec4) == FieldClass::Vector);
    CHECK(FieldClassForColumnKind(TableColumnKind::String) == FieldClass::String);
    CHECK(FieldClassForColumnKind(TableColumnKind::AssetRef) == FieldClass::AssetHandle);
}
