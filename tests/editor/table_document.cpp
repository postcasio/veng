// The table editor panels' authoring rules and save contract, exercised without a frame.
//
// Every rule the panels enforce lives in TableDocument (the ImGui-free document model), so the
// column/row operations, the validation, and — through TableSchemaEditorPanel, whose dependencies
// are a TypeRegistry and a callable — the whole explicit-save contract are checkable here. The
// grid's rendering is not: that needs a live ImGui frame, which this band has no device for.

#include <doctest/doctest.h>

#include <Veng/Asset/DataTable.h>
#include <Veng/Reflection/TypeRegistry.h>

#include "panels/TableDocument.h"
#include "panels/TableSchemaEditorPanel.h"
#include "support/TableTestTypes.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Veng;
using namespace VengEditor;

namespace
{
    nlohmann::json Parse(const string& text)
    {
        return nlohmann::json::parse(text, nullptr, false);
    }

    // The exemplar schema shape: an integer key, a float, a reflected enum, and a string — one
    // fixed-size prefix and one variable-size tail, so the layout has both arms to resolve.
    const char* const SchemaJson = R"({
      "columns": [
        { "name": "id", "type": "Veng::i64" },
        { "name": "weight", "type": "Veng::f32" },
        { "name": "motion", "type": "VengTest::Cadence" },
        { "name": "label", "type": "Veng::string" }
      ],
      "key": "id",
      "note": "hand-authored, must survive a save"
    })";

    const char* const TableJson = R"({
      "schema": "0x00000000000000AB",
      "rows": [
        { "id": 1, "weight": 0.5, "motion": "Steady", "label": "one" },
        { "id": 2, "weight": 1.5, "motion": "Burst", "label": "two" }
      ]
    })";

    path TempFile(const char* name)
    {
        const path file = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove(file);
        return file;
    }

    void WriteFile(const path& file, const string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    string ReadFile(const path& file)
    {
        const std::ifstream in(file, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }
}

TEST_CASE("table schema document resolves through the importer's own layout")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);

    const TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);
    REQUIRE(document.Columns.size() == 4);
    CHECK(document.Key == "id");
    CHECK(document.Columns[2].Type == TypeIdOf<VengTest::Cadence>());

    vector<TableColumnDescriptor> columns;
    const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
    REQUIRE_MESSAGE(layout.has_value(), layout.error());

    CHECK(layout->KeyColumn == 0);
    CHECK(layout->KeyKind == TableKeyKind::Integer);
    // A string column makes the row variable-size, so the string and nothing after it is walked.
    CHECK_FALSE(layout->FixedStride);
    CHECK(columns[0].Offset == 0);
    CHECK(columns[1].Offset == sizeof(i64));
    CHECK(columns[3].Offset == CookedTableColumnOffsetUnresolved);
}

TEST_CASE("table schema document reports what the cook would reject")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);
    vector<TableColumnDescriptor> columns;

    SUBCASE("a duplicated column name")
    {
        TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);
        document.RenameColumn(1, "id");
        const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().find("declared more than once") != string::npos);
    }

    SUBCASE("a key column with no total order")
    {
        TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);
        document.Key = "weight";
        const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().find("must be an integer or a string") != string::npos);
    }

    SUBCASE("a key naming no column")
    {
        TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);
        document.Key = "absent";
        const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().find("key column 'absent' is not declared") != string::npos);
    }

    SUBCASE("an unregistered column type, named by the spelling that failed")
    {
        const TableSchemaDocument document = TableSchemaDocument::Read(
            Parse(R"({"columns":[{"name":"id","type":"Veng::Nope"}],"key":"id"})"), types);
        const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().find("Veng::Nope") != string::npos);
    }
}

TEST_CASE("table schema column operations keep the key reference correct")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);
    TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);

    SUBCASE("adding uniquifies the name against the existing columns")
    {
        const usize first = document.AddColumn("extra", types.Info(TypeIdOf<f32>()));
        const usize second = document.AddColumn("extra", types.Info(TypeIdOf<f32>()));
        CHECK(document.Columns[first].Name == "extra");
        CHECK(document.Columns[second].Name == "extra2");
    }

    SUBCASE("renaming the key column carries the key with it")
    {
        document.RenameColumn(0, "rowId");
        CHECK(document.Key == "rowId");

        vector<TableColumnDescriptor> columns;
        CHECK(document.Resolve(types, columns).has_value());
    }

    SUBCASE("removing the key column clears the key rather than dangling it")
    {
        document.RemoveColumn(0);
        CHECK(document.Key.empty());
        CHECK(document.Columns.size() == 3);
    }

    SUBCASE("reordering moves the cell offsets with the columns")
    {
        // The string column moves to the front, so every following cell loses its constant offset.
        document.MoveColumn(3, 0);
        CHECK(document.Columns[0].Name == "label");

        vector<TableColumnDescriptor> columns;
        const Result<TableSchemaLayout> layout = document.Resolve(types, columns);
        REQUIRE_MESSAGE(layout.has_value(), layout.error());
        CHECK(layout->KeyColumn == 1);
        CHECK(columns[1].Offset == CookedTableColumnOffsetUnresolved);
    }
}

TEST_CASE("table schema save preserves the keys it does not own")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);

    const TableSchemaDocument document = TableSchemaDocument::Read(Parse(SchemaJson), types);
    nlohmann::json doc = Parse(SchemaJson);
    document.Write(doc);

    CHECK(doc["note"] == "hand-authored, must survive a save");
    CHECK(doc["columns"].size() == 4);
    CHECK(doc["columns"][2]["type"] == "VengTest::Cadence");
}

TEST_CASE("table data document binds cells through the walker the cook uses")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);

    const TableSchemaDocument schema = TableSchemaDocument::Read(Parse(SchemaJson), types);
    vector<TableColumnDescriptor> columns;
    REQUIRE(schema.Resolve(types, columns).has_value());

    SUBCASE("a valid table decodes every cell")
    {
        vector<string> diagnostics;
        const TableDataDocument table =
            TableDataDocument::Read(Parse(TableJson), columns, types, diagnostics);

        CHECK(diagnostics.empty());
        REQUIRE(table.Rows.size() == 2);
        CHECK(table.Schema.Value == 0xAB);
        CHECK(*static_cast<const i64*>(table.Rows[0].Cell(0)) == 1);
        CHECK(*static_cast<const VengTest::Cadence*>(table.Rows[1].Cell(2)) ==
              VengTest::Cadence::Burst);
        CHECK(*static_cast<const string*>(table.Rows[1].Cell(3)) == "two");
    }

    SUBCASE("a malformed cell is reported and left default, so the table still opens")
    {
        vector<string> diagnostics;
        const TableDataDocument table =
            TableDataDocument::Read(Parse(R"({"schema":"0x00000000000000AB","rows":[
                     {"id":1,"weight":0.5,"motion":"Frantic","label":"one"}]})"),
                                    columns, types, diagnostics);

        REQUIRE(table.Rows.size() == 1);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].find("Frantic") != string::npos);
        CHECK(*static_cast<const VengTest::Cadence*>(table.Rows[0].Cell(2)) ==
              VengTest::Cadence::Idle);
    }

    SUBCASE("a missing cell is reported per column")
    {
        vector<string> diagnostics;
        const TableDataDocument table =
            TableDataDocument::Read(Parse(R"({"schema":"0x00000000000000AB","rows":[{"id":1}]})"),
                                    columns, types, diagnostics);

        REQUIRE(table.Rows.size() == 1);
        CHECK(diagnostics.size() == 3);
    }
}

TEST_CASE("table data row operations")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);
    const TableSchemaDocument schema = TableSchemaDocument::Read(Parse(SchemaJson), types);
    vector<TableColumnDescriptor> columns;
    REQUIRE(schema.Resolve(types, columns).has_value());

    vector<string> diagnostics;
    TableDataDocument table =
        TableDataDocument::Read(Parse(TableJson), columns, types, diagnostics);
    REQUIRE(diagnostics.empty());

    SUBCASE("an added row is default-constructed in every cell")
    {
        const usize added = table.AddRow(columns, types);
        CHECK(added == 2);
        CHECK(*static_cast<const i64*>(table.Rows[added].Cell(0)) == 0);
        CHECK(static_cast<const string*>(table.Rows[added].Cell(3))->empty());
    }

    SUBCASE("a duplicated row is a value copy inserted directly after its source")
    {
        const usize copy = table.DuplicateRow(0, columns, types);
        CHECK(copy == 1);
        REQUIRE(table.Rows.size() == 3);
        CHECK(*static_cast<const string*>(table.Rows[copy].Cell(3)) == "one");

        // A copy, not an alias: editing one must not move the other.
        *static_cast<string*>(table.Rows[copy].Cell(3)) = "edited";
        CHECK(*static_cast<const string*>(table.Rows[0].Cell(3)) == "one");
    }

    SUBCASE("removing and reordering")
    {
        table.MoveRow(0, 1);
        CHECK(*static_cast<const i64*>(table.Rows[0].Cell(0)) == 2);
        table.RemoveRow(0);
        REQUIRE(table.Rows.size() == 1);
        CHECK(*static_cast<const i64*>(table.Rows[0].Cell(0)) == 1);
    }

    SUBCASE("a duplicated key is flagged on the later row only")
    {
        const usize copy = table.DuplicateRow(0, columns, types);
        const vector<bool> duplicates = table.DuplicateKeys(columns, 0, types);
        REQUIRE(duplicates.size() == 3);
        CHECK_FALSE(duplicates[0]);
        CHECK(duplicates[copy]);

        *static_cast<i64*>(table.Rows[copy].Cell(0)) = 99;
        CHECK_FALSE(table.DuplicateKeys(columns, 0, types)[copy]);
    }
}

TEST_CASE("table data save round-trips values and preserves unknown row keys")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);
    const TableSchemaDocument schema = TableSchemaDocument::Read(Parse(SchemaJson), types);
    vector<TableColumnDescriptor> columns;
    REQUIRE(schema.Resolve(types, columns).has_value());

    const char* const authored = R"({
      "schema": "0x00000000000000AB",
      "comment": "top-level key we do not own",
      "rows": [ { "id": 1, "weight": 0.5, "motion": "Steady", "label": "one", "why": "keep me" } ]
    })";

    vector<string> diagnostics;
    TableDataDocument table = TableDataDocument::Read(Parse(authored), columns, types, diagnostics);
    REQUIRE(diagnostics.empty());

    *static_cast<string*>(table.Rows[0].Cell(3)) = "renamed";

    nlohmann::json doc = Parse(authored);
    table.Write(doc, columns, types);

    CHECK(doc["comment"] == "top-level key we do not own");
    CHECK(doc["rows"][0]["why"] == "keep me");
    CHECK(doc["rows"][0]["label"] == "renamed");
    // Enums round-trip by enumerator name, which is the only spelling the cook's reader accepts.
    CHECK(doc["rows"][0]["motion"] == "Steady");

    // The written document reads back identically — the panel's save is the importer's input.
    vector<string> reread;
    const TableDataDocument again = TableDataDocument::Read(doc, columns, types, reread);
    CHECK(reread.empty());
    CHECK(*static_cast<const string*>(again.Rows[0].Cell(3)) == "renamed");
}

TEST_CASE("the schema editor writes its source only on an explicit save")
{
    TypeRegistry types;
    VengTest::RegisterTableTestTypes(types);

    const path source = TempFile("veng_table_schema_save.tableschema.json");
    WriteFile(source, SchemaJson);
    const string original = ReadFile(source);

    // Completes each cook synchronously (with a failure — the panel needs no mount to be driven
    // here), so the panel's in-flight guard advances exactly as it does against a real backend.
    u32 cooks = 0;
    const auto cookNow =
        [&cooks](const CookRequest&, const function<void(Result<MountHandle>)>& onComplete)
    {
        ++cooks;
        onComplete(std::unexpected(string{"stub cook"}));
    };

    TableSchemaEditorPanel panel(AssetId{0xABULL}, source, types, cookNow);

    // Opening cooks once (to make the asset addressable) and writes nothing.
    CHECK(cooks == 1);
    CHECK_FALSE(panel.HasUnsavedChanges());
    CHECK(ReadFile(source) == original);

    SUBCASE("an unsaved document leaves the file untouched")
    {
        // No frame runs, so nothing marks the panel dirty — which is the point: there is no timer
        // and no path that writes without Save() being called.
        CHECK(ReadFile(source) == original);
        CHECK(cooks == 1);
    }

    SUBCASE("Save writes once, clears the dirty flag, and recooks after the write")
    {
        REQUIRE(panel.Save().has_value());
        CHECK_FALSE(panel.HasUnsavedChanges());
        CHECK(cooks == 2);

        const nlohmann::json written = Parse(ReadFile(source));
        REQUIRE_FALSE(written.is_discarded());
        CHECK(written["key"] == "id");
        CHECK(written["columns"].size() == 4);
        CHECK(written["note"] == "hand-authored, must survive a save");
    }

    SUBCASE("a save landing while a cook is in flight still recooks")
    {
        // Hold the first cook open, so Save's recook has to queue behind it rather than vanish.
        function<void(Result<MountHandle>)> held;
        u32 deferred = 0;
        const path other = TempFile("veng_table_schema_queued.tableschema.json");
        WriteFile(other, SchemaJson);

        TableSchemaEditorPanel queued(
            AssetId{0xACULL}, other, types,
            [&](const CookRequest&, function<void(Result<MountHandle>)> onComplete)
            {
                ++deferred;
                held = std::move(onComplete);
            });
        CHECK(deferred == 1);

        REQUIRE(queued.Save().has_value());
        CHECK(deferred == 1);

        // Landing the held cook releases the queued one.
        held(std::unexpected(string{"stub cook"}));
        CHECK(deferred == 2);

        std::filesystem::remove(other);
    }

    SUBCASE("an unwritable source is a recoverable error, not a crash or a silent loss")
    {
        std::filesystem::remove(source);
        TableSchemaEditorPanel missing(AssetId{0xABULL}, source / "not-a-directory", types,
                                       cookNow);
        CHECK_FALSE(missing.Save().has_value());
    }

    std::filesystem::remove(source);
}
