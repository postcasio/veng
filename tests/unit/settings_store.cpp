// Settings-core store cases: the generalized SettingsStore<TDoc> round-trips discrete + scalar
// choices through JSON, tolerant-migrates an older document, yields defaults on a missing and a
// corrupt file, validates a preset-free schema, and keeps two domains' config files isolated. The
// graphics façade over the same core writes a byte-identical graphics.json (an absolute assertion
// on the on-disk format, captured from the store as it stands). No GPU is touched — a store takes a
// schema Ref, a type registry, and a config path, so it runs without an Application.

#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "support/TempPath.h"

#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Settings/SettingsSchema.h>
#include <Veng/Settings/SettingsStore.h>

using namespace Veng;

namespace
{
    // A schema with both setting kinds and Low/High presets over the discrete pair plus the
    // render-scale built-in; its default preset ("high") equals every setting's own default.
    SettingsSchemaData GraphicsSampleSchema()
    {
        SettingsSchemaData schema;

        SettingsCategory quality;
        quality.Id = "quality";
        quality.Label = "Quality";
        quality.Settings.push_back(SettingsSetting{
            .Id = "shadows",
            .Label = "Shadows",
            .Kind = SettingsSettingKind::Discrete,
            .Options = {{.Id = "off", .Label = "Off"}, {.Id = "high", .Label = "High"}},
            .DefaultOption = 1});
        quality.Settings.push_back(SettingsSetting{.Id = "aa",
                                                   .Label = "Anti-aliasing",
                                                   .Kind = SettingsSettingKind::Discrete,
                                                   .Options = {{.Id = "none", .Label = "None"},
                                                               {.Id = "fxaa", .Label = "FXAA"},
                                                               {.Id = "taa", .Label = "TAA"}},
                                                   .DefaultOption = 2});
        schema.Categories.push_back(std::move(quality));

        SettingsCategory camera;
        camera.Id = "camera";
        camera.Label = "Camera";
        camera.Settings.push_back(SettingsSetting{.Id = "fov",
                                                  .Label = "Field of View",
                                                  .Kind = SettingsSettingKind::Scalar,
                                                  .Min = 60.0f,
                                                  .Max = 110.0f,
                                                  .Step = 1.0f,
                                                  .DefaultValue = 90.0f});
        schema.Categories.push_back(std::move(camera));

        schema.Presets.push_back(
            SettingsPreset{.Id = "low",
                           .Label = "Low",
                           .Entries = {{.SettingId = "shadows", .OptionId = "off"},
                                       {.SettingId = "aa", .OptionId = "none"},
                                       {.SettingId = std::string(GraphicsRenderScaleBuiltinId),
                                        .ScalarValue = 0.75f}}});
        schema.Presets.push_back(
            SettingsPreset{.Id = "high",
                           .Label = "High",
                           .Entries = {{.SettingId = "shadows", .OptionId = "high"},
                                       {.SettingId = "aa", .OptionId = "taa"},
                                       {.SettingId = std::string(GraphicsRenderScaleBuiltinId),
                                        .ScalarValue = 1.0f}}});
        schema.DefaultPreset = "high";
        return schema;
    }

    // A minimal domain schema with no presets — the shape an audio schema takes.
    SettingsSchemaData PresetFreeSchema()
    {
        SettingsSchemaData schema;
        SettingsCategory volume;
        volume.Id = "volume";
        volume.Label = "Volume";
        volume.Settings.push_back(SettingsSetting{
            .Id = "quality",
            .Label = "Quality",
            .Kind = SettingsSettingKind::Discrete,
            .Options = {{.Id = "low", .Label = "Low"}, {.Id = "high", .Label = "High"}},
            .DefaultOption = 1});
        volume.Settings.push_back(SettingsSetting{.Id = "master",
                                                  .Label = "Master",
                                                  .Kind = SettingsSettingKind::Scalar,
                                                  .Min = 0.0f,
                                                  .Max = 1.0f,
                                                  .Step = 0.0f,
                                                  .DefaultValue = 0.8f});
        schema.Categories.push_back(std::move(volume));
        return schema;
    }

    path ConfigPathFor(const std::string& name)
    {
        return TestSupport::TempDir() / name;
    }

    std::string ReadFile(const path& p)
    {
        std::ifstream file(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    }
}

TEST_CASE("SettingsStore: the graphics façade writes a byte-identical graphics.json")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(GraphicsSampleSchema());
    const path configPath = ConfigPathFor("settings_graphics_golden.json");
    std::filesystem::remove(configPath);

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(settings.ApplyPreset("low").has_value());
    settings.SetChosenScalar("fov", 100.0f);
    settings.GetDisplay().Resolution = uvec2{1920, 1080};
    settings.GetDisplay().Fullscreen = FullscreenMode::Borderless;
    settings.GetDisplay().MonitorId = 1;
    settings.GetDisplay().RefreshRateHz = 144;
    settings.GetDisplay().Present = PresentMode::Mailbox;
    settings.GetDisplay().FrameCapHz = 120;
    settings.GetDisplay().Gamma = 1.5f;
    REQUIRE(settings.Save().has_value());

    // The exact on-disk format: keys sorted, two-space indent, no trailing newline. A change to the
    // graphics document's reflection would move these bytes.
    const char* golden = R"({
  "ActivePreset": "low",
  "Choices": [
    {
      "OptionId": "off",
      "ScalarValue": 0.0,
      "SettingId": "shadows"
    },
    {
      "OptionId": "none",
      "ScalarValue": 0.0,
      "SettingId": "aa"
    },
    {
      "OptionId": "",
      "ScalarValue": 100.0,
      "SettingId": "fov"
    }
  ],
  "Display": {
    "Brightness": 1.0,
    "FrameCapHz": 120,
    "Fullscreen": "Borderless",
    "Gamma": 1.5,
    "MonitorId": 1,
    "Present": "Mailbox",
    "RefreshRateHz": 144,
    "RenderScale": 0.75,
    "Resolution": [
      1920,
      1080
    ]
  },
  "Version": 1
})";
    CHECK(ReadFile(configPath) == golden);
}

TEST_CASE("SettingsStore: a generic document round-trips discrete + scalar choices")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(PresetFreeSchema());
    const path configPath = ConfigPathFor("settings_generic_roundtrip.json");
    std::filesystem::remove(configPath);

    {
        SettingsStore<SettingsChoices> store(
            SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        store.SetChosenOption("quality", "low");
        store.SetChosenScalar("master", 0.3f);
        REQUIRE(store.Save().has_value());
    }
    REQUIRE(std::filesystem::exists(configPath));

    SettingsStore<SettingsChoices> reloaded(
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(reloaded.Load().has_value());
    CHECK(reloaded.WasLoadedFromFile());
    CHECK(reloaded.GetChosenOption("quality") == "low");
    CHECK(reloaded.GetChosenScalar("master") == doctest::Approx(0.3f));
    CHECK(reloaded.GetDocument().Version == SettingsChoicesVersion);
}

TEST_CASE("SettingsStore: an older generic document migrates tolerantly")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(PresetFreeSchema());
    const path configPath = ConfigPathFor("settings_generic_migrate.json");

    // Version 0, a stale setting id, and the "master" setting omitted entirely.
    const char* older = R"({
        "Version": 0,
        "ActivePreset": "",
        "Choices": [
            {"SettingId": "quality", "OptionId": "low", "ScalarValue": 0.0},
            {"SettingId": "obsolete", "OptionId": "whatever", "ScalarValue": 0.0}
        ]
    })";
    {
        std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file << older;
    }

    SettingsStore<SettingsChoices> store(
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(store.Load().has_value());

    CHECK(store.GetChosenOption("quality") == "low");
    CHECK(store.GetChosenScalar("master") == doctest::Approx(0.8f)); // omitted → schema default
    for (const SettingsChoice& choice : store.GetDocument().Choices)
    {
        CHECK(choice.SettingId != "obsolete"); // stale id dropped
    }
    CHECK(store.GetDocument().Version == SettingsChoicesVersion); // re-stamped
}

TEST_CASE("SettingsStore: a missing generic file yields defaults, a corrupt one warns")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(PresetFreeSchema());

    SUBCASE("missing")
    {
        const path configPath = ConfigPathFor("settings_generic_missing.json");
        std::filesystem::remove(configPath);
        SettingsStore<SettingsChoices> store(
            SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        REQUIRE(store.Load().has_value());
        CHECK_FALSE(store.WasLoadedFromFile());
        CHECK(store.GetChosenOption("quality") == "high"); // per-setting default
    }

    SUBCASE("corrupt")
    {
        const path configPath = ConfigPathFor("settings_generic_corrupt.json");
        {
            std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
            REQUIRE(file.is_open());
            file << "{ not valid json ]]";
        }
        int warnings = 0;
        Log::SetSink(
            [&warnings](Log::Level level, std::string_view)
            {
                if (level == Log::Level::Warn)
                {
                    ++warnings;
                }
            });
        SettingsStore<SettingsChoices> store(
            SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        const VoidResult loaded = store.Load();
        Log::SetSink(nullptr);

        CHECK_FALSE(loaded.has_value());
        CHECK(warnings >= 1);
        CHECK(store.WasLoadedFromFile()); // a present-but-malformed file is a returning install
        CHECK(store.GetChosenOption("quality") == "high"); // defaults, not a crash
    }
}

TEST_CASE("SettingsStore: a preset-free schema is valid")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(PresetFreeSchema());

    SettingsStore<SettingsChoices> store(
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}});
    store.ResetToDefaults();

    // No presets: there is no active preset, IsCustom is well-defined (true), and every setting
    // reads its own schema default.
    CHECK(store.GetActivePreset().empty());
    CHECK_FALSE(store.MatchingPreset().has_value());
    CHECK(store.IsCustom());
    CHECK(store.GetChosenOption("quality") == "high");
    CHECK(store.GetChosenScalar("master") == doctest::Approx(0.8f));

    // ApplyPreset on a schema with no such preset fails rather than aborting.
    CHECK_FALSE(store.ApplyPreset("anything").has_value());
}

TEST_CASE("SettingsStore: two domains' config files stay isolated")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<SettingsSchema> schema = SettingsSchema::Create(PresetFreeSchema());
    const path pathA = ConfigPathFor("settings_domain_a.json");
    const path pathB = ConfigPathFor("settings_domain_b.json");
    std::filesystem::remove(pathA);
    std::filesystem::remove(pathB);

    {
        SettingsStore<SettingsChoices> a(
            SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = pathA});
        a.SetChosenOption("quality", "low");
        REQUIRE(a.Save().has_value());
    }

    // B has its own (absent) file, so it loads defaults and never sees A's choice.
    SettingsStore<SettingsChoices> b(
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = pathB});
    REQUIRE(b.Load().has_value());
    CHECK_FALSE(b.WasLoadedFromFile());
    CHECK(b.GetChosenOption("quality") == "high");
    CHECK_FALSE(std::filesystem::exists(pathB));

    // A's file reloads its own choice.
    SettingsStore<SettingsChoices> a2(
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = pathA});
    REQUIRE(a2.Load().has_value());
    CHECK(a2.GetChosenOption("quality") == "low");
}
