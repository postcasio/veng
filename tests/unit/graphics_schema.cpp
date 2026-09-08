// GraphicsSchema runtime cases: a representative schema (both setting kinds, several categories,
// presets) round-trips field-for-field through the shared WriteFields/ReadFields encoder; a record
// written by a narrower struct tolerant-loads with the new field defaulted; and a truncated blob
// loads as Corrupt through the ordinary AssetManager path rather than crashing. The reflection
// side is exercised directly, independent of the cooker.

#include <doctest/doctest.h>

#include <cstring>

#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    constexpr AssetId SchemaId{0x6F5C000000000001ULL};

    // A representative schema: a discrete category (two settings, one two-option, one three-option)
    // and a scalar category (a slider), plus Low/High presets over both and the render-scale
    // built-in.
    GraphicsSchemaData SampleSchema()
    {
        GraphicsSchemaData schema;

        GraphicsCategory quality;
        quality.Id = "quality";
        quality.Label = "Quality";
        quality.Settings.push_back(GraphicsSetting{
            .Id = "shadows",
            .Label = "Shadows",
            .Description = "Shadow map quality.",
            .Kind = GraphicsSettingKind::Discrete,
            .Options = {{.Id = "off", .Label = "Off"}, {.Id = "high", .Label = "High"}},
            .DefaultOption = 1});
        quality.Settings.push_back(GraphicsSetting{.Id = "aa",
                                                   .Label = "Anti-aliasing",
                                                   .Kind = GraphicsSettingKind::Discrete,
                                                   .Options = {{.Id = "none", .Label = "None"},
                                                               {.Id = "fxaa", .Label = "FXAA"},
                                                               {.Id = "taa", .Label = "TAA"}},
                                                   .DefaultOption = 2});
        schema.Categories.push_back(std::move(quality));

        GraphicsCategory camera;
        camera.Id = "camera";
        camera.Label = "Camera";
        camera.Settings.push_back(GraphicsSetting{.Id = "fov",
                                                  .Label = "Field of View",
                                                  .Kind = GraphicsSettingKind::Scalar,
                                                  .Min = 60.0f,
                                                  .Max = 110.0f,
                                                  .Step = 1.0f,
                                                  .DefaultValue = 90.0f});
        schema.Categories.push_back(std::move(camera));

        schema.Presets.push_back(
            GraphicsPreset{.Id = "low",
                           .Label = "Low",
                           .Entries = {{.SettingId = "shadows", .OptionId = "off"},
                                       {.SettingId = "aa", .OptionId = "none"},
                                       {.SettingId = std::string(GraphicsRenderScaleBuiltinId),
                                        .ScalarValue = 0.75f}}});
        schema.Presets.push_back(
            GraphicsPreset{.Id = "high",
                           .Label = "High",
                           .Entries = {{.SettingId = "shadows", .OptionId = "high"},
                                       {.SettingId = "aa", .OptionId = "taa"},
                                       {.SettingId = std::string(GraphicsRenderScaleBuiltinId),
                                        .ScalarValue = 1.0f}}});
        schema.DefaultPreset = "high";
        return schema;
    }

    // The engine services a loader case needs; no GPU is touched by the schema loader.
    struct Host
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Manager;

        Host()
        {
            RegisterBuiltinTypes(Types);
            Manager = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE("GraphicsSchema: a representative schema round-trips field-for-field through the record")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const TypeInfo& info = types.Info(TypeIdOf<GraphicsSchemaData>());

    const GraphicsSchemaData authored = SampleSchema();

    vector<u8> record;
    WriteFields(record, &authored, info, types);

    GraphicsSchemaData loaded;
    REQUIRE(ReadFields(record, &loaded, info, types).has_value());

    REQUIRE(loaded.Categories.size() == authored.Categories.size());
    CHECK(loaded.DefaultPreset == "high");

    // Discrete category and its settings.
    const GraphicsCategory& quality = loaded.Categories[0];
    CHECK(quality.Id == "quality");
    REQUIRE(quality.Settings.size() == 2);
    CHECK(quality.Settings[0].Id == "shadows");
    CHECK(quality.Settings[0].Kind == GraphicsSettingKind::Discrete);
    REQUIRE(quality.Settings[0].Options.size() == 2);
    CHECK(quality.Settings[0].Options[1].Id == "high");
    CHECK(quality.Settings[0].DefaultOption == 1);
    REQUIRE(quality.Settings[1].Options.size() == 3);
    CHECK(quality.Settings[1].DefaultOption == 2);

    // Scalar category.
    const GraphicsSetting& fov = loaded.Categories[1].Settings[0];
    CHECK(fov.Kind == GraphicsSettingKind::Scalar);
    CHECK(fov.Min == doctest::Approx(60.0f));
    CHECK(fov.Max == doctest::Approx(110.0f));
    CHECK(fov.DefaultValue == doctest::Approx(90.0f));

    // Presets, including the render-scale built-in entry.
    REQUIRE(loaded.Presets.size() == 2);
    const GraphicsPreset& high = loaded.Presets[1];
    CHECK(high.Id == "high");
    REQUIRE(high.Entries.size() == 3);
    CHECK(high.Entries[0].SettingId == "shadows");
    CHECK(high.Entries[0].OptionId == "high");
    CHECK(high.Entries[2].SettingId == GraphicsRenderScaleBuiltinId);
    CHECK(high.Entries[2].ScalarValue == doctest::Approx(1.0f));

    // The runtime asset class's accessors resolve by id across categories.
    const Ref<GraphicsSchema> asset = GraphicsSchema::Create(std::move(loaded));
    REQUIRE(asset->FindSetting("fov") != nullptr);
    CHECK(asset->FindSetting("fov")->Kind == GraphicsSettingKind::Scalar);
    CHECK(asset->FindSetting("missing") == nullptr);
    REQUIRE(asset->FindPreset("low") != nullptr);
    CHECK(asset->FindCategory("camera") != nullptr);
}

TEST_CASE(
    "GraphicsSchema: a record from a narrower struct tolerant-loads with the new field default")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const TypeInfo& full = types.Info(TypeIdOf<GraphicsSetting>());

    // Encode a setting through a TypeInfo that carries no Description field — exactly a record
    // cooked before the field existed.
    TypeInfo older = full;
    std::erase_if(older.Fields, [](const FieldDescriptor& f) { return f.Name == "Description"; });
    REQUIRE(older.Fields.size() == full.Fields.size() - 1);

    GraphicsSetting authored;
    authored.Id = "shadows";
    authored.Label = "Shadows";
    authored.Description = "dropped by the old encoder";
    authored.Kind = GraphicsSettingKind::Discrete;
    authored.Options.push_back({.Id = "off", .Label = "Off"});
    authored.DefaultOption = 0;

    vector<u8> record;
    WriteFields(record, &authored, older, types);

    // The current loader reads it through the full descriptor; the missing field stays at its
    // default, so an existing cooked schema loads unchanged within CookedSettingsSchemaVersion.
    GraphicsSetting loaded;
    REQUIRE(ReadFields(record, &loaded, full, types).has_value());
    CHECK(loaded.Id == "shadows");
    CHECK(loaded.Description.empty());
    REQUIRE(loaded.Options.size() == 1);
    CHECK(loaded.Options[0].Id == "off");
}

TEST_CASE("GraphicsSchema: a truncated blob is Corrupt, not a crash")
{
    Host host;

    // A well-formed header claiming a record longer than the bytes that follow.
    const GraphicsSchemaData authored = SampleSchema();
    vector<u8> record;
    WriteFields(record, &authored, host.Types.Info(TypeIdOf<GraphicsSchemaData>()), host.Types);

    CookedSettingsSchemaHeader header{};
    header.Version = CookedSettingsSchemaVersion;
    header.RecordBytes = static_cast<u32>(record.size());

    vector<u8> blob;
    const auto* headerBytes = reinterpret_cast<const u8*>(&header);
    blob.insert(blob.end(), headerBytes, headerBytes + sizeof(header));
    // Only half the record survives.
    blob.insert(blob.end(), record.begin(), record.begin() + record.size() / 2);

    ArchiveWriter writer;
    writer.Add(SchemaId, AssetTypes::SettingsSchema, blob);
    const path archivePath =
        Veng::TestSupport::TempDir() / "veng_graphics_schema_truncated.vengpack";
    REQUIRE(writer.Write(archivePath).has_value());
    REQUIRE(host.Manager->Mount(archivePath).has_value());

    const AssetResult<AssetHandle<GraphicsSchema>> loaded =
        host.Manager->LoadSync<GraphicsSchema>(SchemaId);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().Kind == AssetError::Corrupt);
}
