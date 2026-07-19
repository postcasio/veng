// DataTable runtime cases: the cooked schema + table blobs load through the ordinary
// AssetManager path (no GPU — Context is default-constructed and neither loader touches it),
// FindRow hits and misses on both key kinds, the zero-copy column view and the reflected read
// path agree, the typed row bridge binds a whole row, and a blob whose length witnesses disagree
// is rejected as Corrupt rather than asserted on. The blobs are assembled by hand here — through
// the same record encoding the cook uses — so the runtime side is tested independently of the
// cooker.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include "support/TableTestTypes.h"
#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    constexpr AssetId SchemaId{0x05AB1E0000000001ULL};
    constexpr AssetId TableId{0x05AB1E0000000002ULL};
    constexpr AssetId ReferencedId{0x05AB1E0000000003ULL};

    // The fixed-size columns, in the order the fixtures declare them.
    constexpr u32 IdOffset = 0;
    constexpr u32 FlagOffset = 8;
    constexpr u32 WeightOffset = 9;
    constexpr u32 OffsetOffset = 13;
    constexpr u32 MotionOffset = 21;
    constexpr u32 PayloadOffset = 25;
    constexpr u32 FixedRowStride = 33;

    // doctest's message argument is streamed, so the failure text is built by a helper rather
    // than a ternary in the macro argument.
    template <class T>
    string LoadDetail(const AssetResult<T>& result)
    {
        return result.has_value() ? string{} : result.error().Detail;
    }

    template <class T>
    string CellDetail(const Result<T>& result)
    {
        return result.has_value() ? string{} : result.error();
    }

    template <class T>
    void Append(vector<u8>& out, const T& value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }

    // Appends one cell through the shared record encoding, exactly as the importer does.
    template <class T>
    void AppendCell(vector<u8>& out, const TypeRegistry& types, const T& value)
    {
        FieldDescriptor field;
        field.Type = TypeIdOf<T>();
        field.Class = FieldClassOf<T>();
        field.Offset = 0;
        WriteFieldValue(out, &value, field, types);
    }

    struct ColumnSpec
    {
        string Name;
        TypeId Type = InvalidTypeId;
    };

    vector<ColumnSpec> FixedColumns()
    {
        return {{.Name = "Id", .Type = TypeIdOf<i64>()},
                {.Name = "Flag", .Type = TypeIdOf<bool>()},
                {.Name = "Weight", .Type = TypeIdOf<f32>()},
                {.Name = "Offset", .Type = TypeIdOf<vec2>()},
                {.Name = "Motion", .Type = TypeIdOf<VengTest::Cadence>()},
                {.Name = "Payload", .Type = TypeIdOf<AssetHandle<RawAsset>>()}};
    }

    vector<ColumnSpec> MixedColumns()
    {
        vector<ColumnSpec> columns = FixedColumns();
        columns.push_back({.Name = "Label", .Type = TypeIdOf<string>()});
        columns.push_back({.Name = "Extent", .Type = TypeIdOf<VengTest::Extent>()});
        columns.push_back({.Name = "Curve", .Type = TypeIdOf<VengTest::WeightCurve>()});
        return columns;
    }

    // Lays the columns out exactly as the schema cook does, so a fixture blob and a cooked blob
    // are indistinguishable to the loader.
    vector<u8> BuildSchemaBlob(const TypeRegistry& types, const vector<ColumnSpec>& columns,
                               const u32 keyColumn, const TableKeyKind keyKind)
    {
        u32 cursor = 0;
        bool arithmetic = true;
        bool allFixed = true;
        vector<u32> offsets;
        for (const ColumnSpec& column : columns)
        {
            const TypeInfo& info = types.Info(column.Type);
            const bool fixed = TableCellIsFixedSize(info.Class);
            allFixed = allFixed && fixed;
            if (arithmetic && fixed)
            {
                offsets.push_back(cursor);
                cursor += TableCellEncodedSize(info.Class, info);
            }
            else
            {
                arithmetic = false;
                offsets.push_back(CookedTableColumnOffsetUnresolved);
            }
        }

        CookedTableSchemaHeader header{};
        header.Version = CookedTableSchemaVersion;
        header.ColumnCount = static_cast<u32>(columns.size());
        header.KeyColumn = keyColumn;
        header.KeyKind = static_cast<u32>(keyKind);
        header.RowStride = allFixed ? cursor : 0;
        header.FixedStride = allFixed ? 1u : 0u;

        vector<u8> blob;
        Append(blob, header);
        for (usize i = 0; i < columns.size(); ++i)
        {
            CookedTableColumn cooked{};
            std::memcpy(cooked.Name, columns[i].Name.data(), columns[i].Name.size());
            cooked.Type = columns[i].Type;
            cooked.Offset = offsets[i];
            Append(blob, cooked);
        }
        return blob;
    }

    // One row's fixed-size prefix, shared by the fixed-stride and variable-size fixtures so the
    // two are byte-identical over the columns they share.
    vector<u8> BuildFixedPrefix(const TypeRegistry& types, const i64 id, const bool flag,
                                const f32 weight, const vec2 offset, const VengTest::Cadence motion,
                                const u64 payload)
    {
        vector<u8> row;
        AppendCell(row, types, id);
        AppendCell(row, types, flag);
        AppendCell(row, types, weight);
        AppendCell(row, types, offset);
        AppendCell(row, types, motion);
        // An asset-handle cell is its leading AssetId and nothing else.
        Append(row, payload);
        return row;
    }

    struct TableBlobParts
    {
        vector<u8> Rows;
        vector<u32> RowOffsets;
        vector<CookedTableKey> Keys;
        vector<u8> KeyHeap;
        bool FixedStride = true;
        u32 RowStride = FixedRowStride;
        TableKeyKind KeyKind = TableKeyKind::Integer;
    };

    vector<u8> AssembleTableBlob(const TableBlobParts& parts)
    {
        CookedDataTableHeader header{};
        header.SchemaId = SchemaId.Value;
        header.Version = CookedDataTableVersion;
        header.RowCount = static_cast<u32>(parts.Keys.size());
        header.FixedStride = parts.FixedStride ? 1u : 0u;
        header.RowStride = parts.FixedStride ? parts.RowStride : 0;
        header.RowBytes = static_cast<u32>(parts.Rows.size());
        header.KeyHeapBytes = static_cast<u32>(parts.KeyHeap.size());
        header.KeyKind = static_cast<u32>(parts.KeyKind);

        vector<u8> blob;
        Append(blob, header);
        for (const CookedTableKey& key : parts.Keys)
        {
            Append(blob, key);
        }
        if (!parts.FixedStride)
        {
            for (const u32 offset : parts.RowOffsets)
            {
                Append(blob, offset);
            }
        }
        blob.insert(blob.end(), parts.Rows.begin(), parts.Rows.end());
        blob.insert(blob.end(), parts.KeyHeap.begin(), parts.KeyHeap.end());
        return blob;
    }

    // Two rows keyed 10 and 40, all columns fixed-size: no directory, arithmetic addressing.
    TableBlobParts BuildFixedParts(const TypeRegistry& types)
    {
        TableBlobParts parts;
        const vector<u8> first = BuildFixedPrefix(types, 10, true, 2.5f, vec2(1.0f, 2.0f),
                                                  VengTest::Cadence::Burst, ReferencedId.Value);
        const vector<u8> second = BuildFixedPrefix(types, 40, false, -1.25f, vec2(3.0f, 4.0f),
                                                   VengTest::Cadence::Idle, 0);
        REQUIRE(first.size() == FixedRowStride);
        parts.Rows.insert(parts.Rows.end(), first.begin(), first.end());
        parts.Rows.insert(parts.Rows.end(), second.begin(), second.end());
        parts.Keys.push_back({.IntKey = 10, .StringKey = {}, .RowIndex = 0, .Pad = 0});
        parts.Keys.push_back({.IntKey = 40, .StringKey = {}, .RowIndex = 1, .Pad = 0});
        return parts;
    }

    // The same two rows plus a string, a nested struct, and an array — variable-size, so the
    // blob carries a row directory.
    TableBlobParts BuildMixedParts(const TypeRegistry& types)
    {
        TableBlobParts parts;
        parts.FixedStride = false;

        const auto appendRow = [&parts, &types](const vector<u8>& prefix, const string& label,
                                                const VengTest::Extent& extent,
                                                const vector<f32>& samples)
        {
            parts.RowOffsets.push_back(static_cast<u32>(parts.Rows.size()));
            parts.Rows.insert(parts.Rows.end(), prefix.begin(), prefix.end());
            AppendCell(parts.Rows, types, label);
            AppendCell(parts.Rows, types, extent);
            AppendCell(parts.Rows, types, VengTest::WeightCurve{.Samples = samples});
        };

        appendRow(BuildFixedPrefix(types, 10, true, 2.5f, vec2(1.0f, 2.0f),
                                   VengTest::Cadence::Burst, ReferencedId.Value),
                  "first", VengTest::Extent{.Size = vec2(8.0f, 4.0f), .Margin = 0.5f},
                  {1.0f, 2.0f, 3.0f});
        appendRow(BuildFixedPrefix(types, 40, false, -1.25f, vec2(3.0f, 4.0f),
                                   VengTest::Cadence::Idle, 0),
                  "second", VengTest::Extent{.Size = vec2(2.0f, 1.0f), .Margin = 0.25f}, {});

        parts.Keys.push_back({.IntKey = 10, .StringKey = {}, .RowIndex = 0, .Pad = 0});
        parts.Keys.push_back({.IntKey = 40, .StringKey = {}, .RowIndex = 1, .Pad = 0});
        return parts;
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

    // The engine services a loader case needs; no GPU is touched by either table loader.
    struct Host
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Manager;

        Host()
        {
            VengTest::RegisterTableTestTypes(Types);
            Manager = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE("DataTable: a fixed-stride table decodes every column through the zero-copy view")
{
    Host host;
    const path archivePath =
        WriteFixtureArchive("veng_data_table_fixed",
                            BuildSchemaBlob(host.Types, FixedColumns(), 0, TableKeyKind::Integer),
                            AssembleTableBlob(BuildFixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
    REQUIRE_MESSAGE(loaded.has_value(), LoadDetail(loaded));

    const AssetHandle<DataTable>& table = *loaded;
    CHECK(table->GetRowCount() == 2);
    CHECK(table->GetKeyKind() == TableKeyKind::Integer);

    // The schema streamed in as an ordinary dependency, so the table resolves columns by name.
    const TableSchema& schema = table->GetSchema();
    CHECK(schema.GetColumns().size() == 6);
    CHECK(schema.IsFixedStride());
    CHECK(schema.GetRowStride() == FixedRowStride);
    CHECK(schema.GetKeyColumn().Name == "Id");
    CHECK(schema.FindColumn("Weight") != nullptr);
    CHECK(schema.FindColumn("absent") == nullptr);
    CHECK(schema.FindColumn("Offset")->Offset == OffsetOffset);
    CHECK(schema.FindColumn("Motion")->Class == FieldClass::Enum);
    CHECK(schema.FindColumn("Payload")->Offset == PayloadOffset);
    CHECK(schema.FindColumn("Id")->Offset == IdOffset);
    CHECK(schema.FindColumn("Flag")->Offset == FlagOffset);
    CHECK(schema.FindColumn("Weight")->Offset == WeightOffset);
    CHECK(schema.FindColumn("Motion")->Offset == MotionOffset);

    const TableColumn<i64> id = table->GetColumn<i64>("Id");
    const TableColumn<bool> flag = table->GetColumn<bool>("Flag");
    const TableColumn<f32> weight = table->GetColumn<f32>("Weight");
    const TableColumn<vec2> offset = table->GetColumn<vec2>("Offset");
    const TableColumn<VengTest::Cadence> motion = table->GetColumn<VengTest::Cadence>("Motion");
    const TableColumn<AssetId> payload = table->GetAssetIdColumn("Payload");

    CHECK(id[0] == 10);
    CHECK(flag[0]);
    CHECK(weight[0] == doctest::Approx(2.5f));
    CHECK(offset[0].x == doctest::Approx(1.0f));
    CHECK(offset[0].y == doctest::Approx(2.0f));
    CHECK(motion[0] == VengTest::Cadence::Burst);
    CHECK(payload[0] == ReferencedId);

    CHECK(id[1] == 40);
    CHECK_FALSE(flag[1]);
    CHECK(motion[1] == VengTest::Cadence::Idle);
    // The unset reference stays invalid: a table holds an id, never a handle, and loads nothing.
    CHECK_FALSE(payload[1].IsValid());

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: a row-directory table reads its fixed columns identically to a "
          "fixed-stride one")
{
    Host host;
    const path archivePath =
        WriteFixtureArchive("veng_data_table_mixed",
                            BuildSchemaBlob(host.Types, MixedColumns(), 0, TableKeyKind::Integer),
                            AssembleTableBlob(BuildMixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
    REQUIRE_MESSAGE(loaded.has_value(), LoadDetail(loaded));
    const AssetHandle<DataTable>& table = *loaded;

    CHECK_FALSE(table->GetSchema().IsFixedStride());
    CHECK(table->GetRowCount() == 2);

    // The accessor API does not branch on addressing mode: the same reads yield the same values.
    CHECK(table->GetColumn<i64>("Id")[0] == 10);
    CHECK(table->GetColumn<i64>("Id")[1] == 40);
    CHECK(table->GetColumn<f32>("Weight")[0] == doctest::Approx(2.5f));
    CHECK(table->GetColumn<VengTest::Cadence>("Motion")[0] == VengTest::Cadence::Burst);
    CHECK(table->GetAssetIdColumn("Payload")[0] == ReferencedId);

    // The rows genuinely differ in size, so this is the directory path and not a stride in disguise.
    CHECK(table->GetRowBytes(0).size() != table->GetRowBytes(1).size());

    // A string cell reads zero-copy out of the row itself.
    const Result<std::string_view> label = table->GetStringCell(0, "Label");
    REQUIRE_MESSAGE(label.has_value(), CellDetail(label));
    CHECK(*label == "first");
    CHECK(*table->GetStringCell(1, "Label") == "second");

    // A nested-struct column decodes through the reflected read path.
    VengTest::Extent extent;
    REQUIRE(table->ReadCell(0, "Extent", extent).has_value());
    CHECK(extent.Size.x == doctest::Approx(8.0f));
    CHECK(extent.Margin == doctest::Approx(0.5f));

    // An array column round-trips its element count and values.
    VengTest::WeightCurve curve;
    REQUIRE(table->ReadCell(0, "Curve", curve).has_value());
    REQUIRE(curve.Samples.size() == 3);
    CHECK(curve.Samples[2] == doctest::Approx(3.0f));

    VengTest::WeightCurve empty;
    REQUIRE(table->ReadCell(1, "Curve", empty).has_value());
    CHECK(empty.Samples.empty());

    // And the reflected read path agrees with the zero-copy view on a fixed column.
    i64 id = 0;
    REQUIRE(table->ReadCell(1, "Id", id).has_value());
    CHECK(id == table->GetColumn<i64>("Id")[1]);

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: the typed row bridge binds a whole row into a reflected struct")
{
    Host host;
    const path archivePath =
        WriteFixtureArchive("veng_data_table_readrow",
                            BuildSchemaBlob(host.Types, MixedColumns(), 0, TableKeyKind::Integer),
                            AssembleTableBlob(BuildMixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
    REQUIRE_MESSAGE(loaded.has_value(), LoadDetail(loaded));
    const AssetHandle<DataTable>& table = *loaded;

    // TuningRow declares four of the nine columns; the rest are decoded and discarded.
    VengTest::TuningRow row;
    REQUIRE(table->ReadRow(0, row).has_value());
    CHECK(row.Id == 10);
    CHECK(row.Label == "first");
    CHECK(row.Weight == doctest::Approx(2.5f));
    CHECK(row.Motion == VengTest::Cadence::Burst);

    VengTest::TuningRow second;
    REQUIRE(table->ReadRow(1, second).has_value());
    CHECK(second.Id == 40);
    CHECK(second.Label == "second");
    CHECK(second.Motion == VengTest::Cadence::Idle);

    std::filesystem::remove(archivePath);
}

TEST_CASE("DataTable: FindRow hits and misses on an integer key")
{
    Host host;
    const path archivePath =
        WriteFixtureArchive("veng_data_table_findrow",
                            BuildSchemaBlob(host.Types, FixedColumns(), 0, TableKeyKind::Integer),
                            AssembleTableBlob(BuildFixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
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
    Host host;

    // The key column is "Label", so the key index carries its own heap of the key strings.
    TableBlobParts parts = BuildMixedParts(host.Types);
    parts.KeyHeap.assign({'f', 'i', 'r', 's', 't', 's', 'e', 'c', 'o', 'n', 'd'});
    parts.KeyKind = TableKeyKind::String;
    parts.Keys = {
        {.IntKey = 0, .StringKey = {.Offset = 0, .Length = 5}, .RowIndex = 0, .Pad = 0},
        {.IntKey = 0, .StringKey = {.Offset = 5, .Length = 6}, .RowIndex = 1, .Pad = 0},
    };

    const path archivePath =
        WriteFixtureArchive("veng_data_table_stringkey",
                            BuildSchemaBlob(host.Types, MixedColumns(), 6, TableKeyKind::String),
                            AssembleTableBlob(parts));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
    REQUIRE_MESSAGE(loaded.has_value(), LoadDetail(loaded));
    const AssetHandle<DataTable>& table = *loaded;

    CHECK(table->GetKeyKind() == TableKeyKind::String);
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
    Host host;
    vector<u8> truncated = AssembleTableBlob(BuildFixedParts(host.Types));
    truncated.resize(sizeof(CookedDataTableHeader) + 4);

    const path archivePath = WriteFixtureArchive(
        "veng_data_table_truncated",
        BuildSchemaBlob(host.Types, FixedColumns(), 0, TableKeyKind::Integer), truncated);
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<DataTable>> loaded = host.Manager->LoadSync<DataTable>(TableId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);

    std::filesystem::remove(archivePath);
}

// --- The length witnesses ---------------------------------------------------
//
// GetRowCount() reports the key index's length, so a blob whose row region or directory disagrees
// with it would read past (or short of) the rows it claims. Each disagreement is a data condition
// on disk, so each must surface as Corrupt rather than an assert.

namespace
{
    // Loads a hand-corrupted table blob and returns the load error, if any.
    optional<AssetLoadError> LoadCorrupted(Host& host, const string& name,
                                           const vector<u8>& tableBlob)
    {
        const path archivePath = WriteFixtureArchive(
            name, BuildSchemaBlob(host.Types, FixedColumns(), 0, TableKeyKind::Integer), tableBlob);
        REQUIRE(host.Manager->Mount(archivePath).has_value());

        const AssetResult<AssetHandle<DataTable>> loaded =
            host.Manager->LoadSync<DataTable>(TableId);
        std::filesystem::remove(archivePath);
        if (loaded.has_value())
        {
            return std::nullopt;
        }
        return loaded.error();
    }

    void PatchHeader(vector<u8>& blob, const function<void(CookedDataTableHeader&)>& patch)
    {
        CookedDataTableHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        patch(header);
        std::memcpy(blob.data(), &header, sizeof(header));
    }
}

TEST_CASE("DataTable: a row region that is not a whole number of fixed-stride rows is Corrupt")
{
    Host host;
    vector<u8> blob = AssembleTableBlob(BuildFixedParts(host.Types));
    PatchHeader(blob, [](CookedDataTableHeader& header) { header.RowBytes -= 1; });

    const optional<AssetLoadError> error = LoadCorrupted(host, "veng_data_table_ragged", blob);
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("whole number") != string::npos);
}

TEST_CASE("DataTable: a row region holding a different row count than the key index is Corrupt")
{
    Host host;
    TableBlobParts parts = BuildFixedParts(host.Types);
    // A third row's worth of bytes with no third key entry.
    parts.Rows.resize(parts.Rows.size() + FixedRowStride);

    const optional<AssetLoadError> error =
        LoadCorrupted(host, "veng_data_table_rowcount", AssembleTableBlob(parts));
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("the key index holds") != string::npos);
}

TEST_CASE("DataTable: a key addressing a row outside the row count is Corrupt")
{
    Host host;
    TableBlobParts parts = BuildFixedParts(host.Types);
    parts.Keys[1].RowIndex = 7;

    const optional<AssetLoadError> error =
        LoadCorrupted(host, "veng_data_table_badrowindex", AssembleTableBlob(parts));
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("addresses row 7") != string::npos);
}

TEST_CASE("DataTable: a row directory that does not start at zero is Corrupt")
{
    Host host;
    TableBlobParts parts = BuildMixedParts(host.Types);
    parts.RowOffsets[0] = 4;

    const optional<AssetLoadError> error =
        LoadCorrupted(host, "veng_data_table_dirstart", AssembleTableBlob(parts));
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("starts at offset 4") != string::npos);
}

TEST_CASE("DataTable: a row directory that steps backwards is Corrupt")
{
    Host host;
    TableBlobParts parts = BuildMixedParts(host.Types);
    REQUIRE(parts.RowOffsets.size() == 2);

    // A third entry is needed for a backwards step that is not also a non-zero first entry.
    parts.RowOffsets.push_back(parts.RowOffsets[1] - 1);
    parts.Keys.push_back({.IntKey = 70, .StringKey = {}, .RowIndex = 2, .Pad = 0});

    const optional<AssetLoadError> error =
        LoadCorrupted(host, "veng_data_table_dirbackwards", AssembleTableBlob(parts));
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("steps backwards") != string::npos);
}

TEST_CASE("DataTable: a row directory entry past the row region is Corrupt")
{
    Host host;
    TableBlobParts parts = BuildMixedParts(host.Types);
    parts.RowOffsets.back() = static_cast<u32>(parts.Rows.size()) + 16;

    const optional<AssetLoadError> error =
        LoadCorrupted(host, "veng_data_table_dirpast", AssembleTableBlob(parts));
    REQUIRE(error.has_value());
    CHECK(error->Kind == AssetError::Corrupt);
    CHECK(error->Detail.find("past the") != string::npos);
}

TEST_CASE("TableSchema: a version-mismatched schema blob is Corrupt")
{
    Host host;
    vector<u8> schemaBlob = BuildSchemaBlob(host.Types, FixedColumns(), 0, TableKeyKind::Integer);
    CookedTableSchemaHeader header{};
    std::memcpy(&header, schemaBlob.data(), sizeof(header));
    header.Version = CookedTableSchemaVersion + 1;
    std::memcpy(schemaBlob.data(), &header, sizeof(header));

    const path archivePath = WriteFixtureArchive("veng_data_table_badversion", schemaBlob,
                                                 AssembleTableBlob(BuildFixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<TableSchema>> loaded =
        host.Manager->LoadSync<TableSchema>(SchemaId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);

    std::filesystem::remove(archivePath);
}

TEST_CASE("TableSchema: a column naming a type this host has not registered is Corrupt")
{
    Host host;
    vector<ColumnSpec> columns = FixedColumns();
    columns[0].Type = 0x00000000DEADBEEFULL;

    // Lay the blob out by hand: the unregistered type has no width to derive one from.
    CookedTableSchemaHeader header{};
    header.Version = CookedTableSchemaVersion;
    header.ColumnCount = static_cast<u32>(columns.size());
    header.KeyColumn = 0;
    header.KeyKind = static_cast<u32>(TableKeyKind::Integer);
    header.FixedStride = 1;
    header.RowStride = FixedRowStride;

    vector<u8> schemaBlob;
    Append(schemaBlob, header);
    for (const ColumnSpec& column : columns)
    {
        CookedTableColumn cooked{};
        std::memcpy(cooked.Name, column.Name.data(), column.Name.size());
        cooked.Type = column.Type;
        Append(schemaBlob, cooked);
    }

    const path archivePath = WriteFixtureArchive("veng_data_table_unregistered", schemaBlob,
                                                 AssembleTableBlob(BuildFixedParts(host.Types)));
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<TableSchema>> loaded =
        host.Manager->LoadSync<TableSchema>(SchemaId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);
    CHECK(loaded.error().Detail.find("does not have registered") != string::npos);

    std::filesystem::remove(archivePath);
}

TEST_CASE("Table columns: only the flat-encoded field classes are fixed-size")
{
    CHECK(TableCellIsFixedSize(FieldClass::Scalar));
    CHECK(TableCellIsFixedSize(FieldClass::Vector));
    CHECK(TableCellIsFixedSize(FieldClass::Quaternion));
    CHECK(TableCellIsFixedSize(FieldClass::Matrix));
    CHECK(TableCellIsFixedSize(FieldClass::Enum));
    CHECK(TableCellIsFixedSize(FieldClass::AssetHandle));
    CHECK_FALSE(TableCellIsFixedSize(FieldClass::String));
    CHECK_FALSE(TableCellIsFixedSize(FieldClass::Struct));
    CHECK_FALSE(TableCellIsFixedSize(FieldClass::Variant));
    CHECK_FALSE(TableCellIsFixedSize(FieldClass::Array));
}

TEST_CASE("Table key columns: only ordered, stably-encoded types may key a table")
{
    CHECK(TableKeyKindForType(TypeIdOf<i64>(), FieldClass::Scalar) == TableKeyKind::Integer);
    CHECK(TableKeyKindForType(TypeIdOf<i32>(), FieldClass::Scalar) == TableKeyKind::Integer);
    CHECK(TableKeyKindForType(TypeIdOf<u32>(), FieldClass::Scalar) == TableKeyKind::Integer);
    CHECK(TableKeyKindForType(TypeIdOf<u8>(), FieldClass::Scalar) == TableKeyKind::Integer);
    CHECK(TableKeyKindForType(TypeIdOf<string>(), FieldClass::String) == TableKeyKind::String);

    // A float cannot be equality-keyed, a bool cannot key more than two rows, and a u64 above
    // 2^63 would sort as negative through the index's i64 key.
    CHECK_FALSE(TableKeyKindForType(TypeIdOf<f32>(), FieldClass::Scalar).has_value());
    CHECK_FALSE(TableKeyKindForType(TypeIdOf<bool>(), FieldClass::Scalar).has_value());
    CHECK_FALSE(TableKeyKindForType(TypeIdOf<u64>(), FieldClass::Scalar).has_value());
    CHECK_FALSE(TableKeyKindForType(TypeIdOf<vec2>(), FieldClass::Vector).has_value());
}
