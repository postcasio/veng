// GraphicsSettings store cases: choices round-trip through the JSON file; an older document
// tolerant-migrates (a stale setting id dropped, an added setting reading its schema default); a
// missing file yields defaults and a corrupt one yields defaults plus a warning; applying a preset
// touches only the preset-eligible settings and leaves the display-identity built-ins alone; the
// Custom query flips after one knob moves off a preset; reset restores the default preset; and an
// atomic Save that cannot complete leaves the previous file intact. No GPU is touched — the store
// takes a schema Ref, a type registry, and a config path, so it runs without an Application.

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "support/TempPath.h"

#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Scene/BuiltinTypes.h>

using namespace Veng;

namespace
{
    // A schema whose default preset ("high") equals every setting's own schema default, so a reset
    // both restores the default preset and reads back each setting's schema default. fov is a scalar
    // setting no preset names — a knob outside the preset matrix.
    GraphicsSchemaData SampleSchema()
    {
        GraphicsSchemaData schema;

        GraphicsCategory quality;
        quality.Id = "quality";
        quality.Label = "Quality";
        quality.Settings.push_back(GraphicsSetting{
            .Id = "shadows",
            .Label = "Shadows",
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

    // A per-test config path under the process scratch dir; the file need not pre-exist.
    path ConfigPathFor(const std::string& name)
    {
        return TestSupport::TempDir() / name;
    }
}

TEST_CASE("GraphicsSettings: choices round-trip through the config file")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());
    const path configPath = ConfigPathFor("graphics_roundtrip.json");
    std::filesystem::remove(configPath);

    {
        GraphicsSettings settings(GraphicsSettingsInfo{
            .Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
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
    }

    GraphicsSettings reloaded(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(reloaded.Load().has_value());

    CHECK(reloaded.GetActivePreset() == "low");
    CHECK(reloaded.GetChosenOption("shadows") == "off");
    CHECK(reloaded.GetChosenOption("aa") == "none");
    CHECK(reloaded.GetChosenScalar("fov") == doctest::Approx(100.0f));
    CHECK(reloaded.GetDisplay().Resolution == uvec2{1920, 1080});
    CHECK(reloaded.GetDisplay().Fullscreen == FullscreenMode::Borderless);
    CHECK(reloaded.GetDisplay().MonitorId == 1);
    CHECK(reloaded.GetDisplay().RefreshRateHz == 144);
    CHECK(reloaded.GetDisplay().Present == PresentMode::Mailbox);
    CHECK(reloaded.GetDisplay().FrameCapHz == 120);
    CHECK(reloaded.GetDisplay().Gamma == doctest::Approx(1.5f));
    CHECK(reloaded.GetChoices().Version == GraphicsChoicesVersion);
}

TEST_CASE("GraphicsSettings: an older document migrates tolerantly")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());
    const path configPath = ConfigPathFor("graphics_migrate.json");

    // A document at version 0 that carries a stale setting id and omits the "aa" setting entirely.
    const char* older = R"({
        "Version": 0,
        "ActivePreset": "high",
        "Choices": [
            {"SettingId": "shadows", "OptionId": "off", "ScalarValue": 0.0},
            {"SettingId": "obsolete", "OptionId": "whatever", "ScalarValue": 0.0}
        ],
        "Display": { "Resolution": [1280, 720] }
    })";
    {
        std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file << older;
    }

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(settings.Load().has_value());

    // The present value loads, the stale id is dropped, the omitted setting reads its schema
    // default, and the document is re-stamped to the current version.
    CHECK(settings.GetChosenOption("shadows") == "off");
    CHECK(settings.GetChosenScalar("fov") == doctest::Approx(90.0f));
    CHECK(settings.GetChosenOption("aa") == "taa");
    for (const GraphicsChoice& choice : settings.GetChoices().Choices)
    {
        CHECK(choice.SettingId != "obsolete");
    }
    CHECK(settings.GetChoices().Version == GraphicsChoicesVersion);
    CHECK(settings.GetDisplay().Resolution == uvec2{1280, 720});
}

TEST_CASE("GraphicsSettings: a missing file yields schema defaults")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());
    const path configPath = ConfigPathFor("graphics_missing.json");
    std::filesystem::remove(configPath);

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    REQUIRE(settings.Load().has_value());

    CHECK(settings.GetActivePreset() == "high");
    CHECK_FALSE(settings.IsCustom());
    CHECK(settings.GetChosenOption("shadows") == "high");
}

TEST_CASE("GraphicsSettings: a corrupt file falls back to defaults with a warning")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());
    const path configPath = ConfigPathFor("graphics_corrupt.json");
    {
        std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file << "{ this is not valid json ]]";
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
    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
    const VoidResult loaded = settings.Load();
    Log::SetSink(nullptr);

    CHECK_FALSE(loaded.has_value());
    CHECK(warnings >= 1);
    // Defaults, not a crash: the schema default preset is active.
    CHECK(settings.GetActivePreset() == "high");
    CHECK(settings.GetChosenOption("shadows") == "high");
}

TEST_CASE("GraphicsSettings: applying a preset leaves the display-identity built-ins untouched")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}});

    // Author display-identity values a preset must never disturb.
    settings.GetDisplay().Resolution = uvec2{2560, 1440};
    settings.GetDisplay().Fullscreen = FullscreenMode::Exclusive;
    settings.GetDisplay().MonitorId = 2;
    settings.GetDisplay().RefreshRateHz = 240;
    settings.GetDisplay().Present = PresentMode::Immediate;
    settings.GetDisplay().FrameCapHz = 90;
    settings.GetDisplay().Brightness = 1.25f;
    settings.GetDisplay().Gamma = 1.1f;

    REQUIRE(settings.ApplyPreset("low").has_value());

    // The one built-in a preset may set moved; every display-identity field is unchanged.
    CHECK(settings.GetDisplay().RenderScale == doctest::Approx(0.75f));
    CHECK(settings.GetDisplay().Resolution == uvec2{2560, 1440});
    CHECK(settings.GetDisplay().Fullscreen == FullscreenMode::Exclusive);
    CHECK(settings.GetDisplay().MonitorId == 2);
    CHECK(settings.GetDisplay().RefreshRateHz == 240);
    CHECK(settings.GetDisplay().Present == PresentMode::Immediate);
    CHECK(settings.GetDisplay().FrameCapHz == 90);
    CHECK(settings.GetDisplay().Brightness == doctest::Approx(1.25f));
    CHECK(settings.GetDisplay().Gamma == doctest::Approx(1.1f));

    // The chosen schema settings are the preset's.
    CHECK(settings.GetChosenOption("shadows") == "off");
    CHECK(settings.GetChosenOption("aa") == "none");
}

TEST_CASE("GraphicsSettings: Custom flips after one knob moves off a preset")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}});

    REQUIRE(settings.ApplyPreset("high").has_value());
    CHECK_FALSE(settings.IsCustom());
    REQUIRE(settings.MatchingPreset().has_value());
    CHECK(*settings.MatchingPreset() == "high");

    settings.SetChosenOption("shadows", "off");
    CHECK(settings.IsCustom());
    CHECK_FALSE(settings.MatchingPreset().has_value());
}

TEST_CASE("GraphicsSettings: reset restores the default preset without firing Custom")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());

    GraphicsSettings settings(
        GraphicsSettingsInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}});

    // Drift away from every default: change a setting and move the display-identity built-ins.
    REQUIRE(settings.ApplyPreset("low").has_value());
    settings.SetChosenOption("shadows", "high");
    settings.GetDisplay().Resolution = uvec2{800, 600};
    settings.GetDisplay().Fullscreen = FullscreenMode::Exclusive;

    settings.ResetToDefaults();

    CHECK(settings.GetActivePreset() == "high");
    CHECK_FALSE(settings.IsCustom());
    CHECK(settings.GetChosenOption("shadows") == "high");
    CHECK(settings.GetChosenOption("aa") == "taa");
    CHECK(settings.GetChosenScalar("fov") == doctest::Approx(90.0f));
    // The display-identity built-ins are back to the engine defaults.
    CHECK(settings.GetDisplay().Resolution == uvec2{0, 0});
    CHECK(settings.GetDisplay().Fullscreen == FullscreenMode::Windowed);
    CHECK(settings.GetDisplay().Present == PresentMode::Vsync);
    CHECK(settings.GetDisplay().RenderScale == doctest::Approx(1.0f));
}

TEST_CASE("GraphicsSettings: a save that cannot complete leaves the previous file intact")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Ref<GraphicsSchema> schema = GraphicsSchema::Create(SampleSchema());

    // A dedicated directory so the read-only simulation touches nothing else.
    const path dir = TestSupport::TempDir() / "graphics_atomic";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const path configPath = dir / "graphics.json";

    {
        GraphicsSettings settings(GraphicsSettingsInfo{
            .Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        REQUIRE(settings.ApplyPreset("low").has_value());
        REQUIRE(settings.Save().has_value());
    }
    REQUIRE(std::filesystem::exists(configPath));

    // Make the directory unwritable so the atomic temp cannot be created; a Save must then fail and
    // leave the good file untouched. If the process can still write (e.g. running as root), the
    // simulation cannot be set up, so the failure assertions are skipped rather than made flaky.
    namespace fs = std::filesystem;
    fs::permissions(dir,
                    fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    bool stillWritable = false;
    {
        const std::ofstream probe(dir / ".probe", std::ios::binary);
        stillWritable = probe.is_open();
    }

    if (stillWritable)
    {
        std::error_code ec;
        fs::remove(dir / ".probe", ec);
        MESSAGE("directory could not be made read-only (likely running as root); skipping the "
                "interrupted-save assertions");
    }
    else
    {
        GraphicsSettings settings(GraphicsSettingsInfo{
            .Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        settings.ApplyPreset("high");
        CHECK_FALSE(settings.Save().has_value());

        GraphicsSettings reloaded(GraphicsSettingsInfo{
            .Schema = schema.get(), .Types = &types, .ConfigPath = configPath});
        REQUIRE(reloaded.Load().has_value());
        CHECK(reloaded.GetActivePreset() == "low");
    }

    fs::permissions(dir, fs::perms::all, fs::perm_options::replace);
    std::filesystem::remove_all(dir);
}
