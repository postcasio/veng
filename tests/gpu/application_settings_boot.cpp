// What the settings boot has finished by the time a consumer's OnInitialize runs: all three
// per-machine stores — graphics, audio, language — are constructed and loaded, and the localization
// service is already the index-backed one rather than the null-object.
//
// The ordering is the whole claim, and only a real Application can witness it: the stores are built
// inside Run(), so a unit case over a bare SettingsStore proves nothing about *when*, and a consumer
// that reads them one frame later cannot tell a boot load from its own. So this drives a headless
// managed-world Application through Run() and snapshots every store from inside OnInitialize.
//
// The stores must have something to load, or "loaded" is unfalsifiable: a first run leaves defaults
// and WasLoadedFromFile() false either way. The case therefore writes a real settings file per
// domain under the app's own config directory first — through the same stores the engine uses, so
// the format is never hand-rolled — and asserts OnInitialize reads those values back. The directory
// is keyed to a name no other test or install uses and is removed afterwards.
//
// It rides the gpu band because Application owns a Context; the assertions touch no device.

#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "support/BootstrapFixture.h"
#include "support/TempPath.h"

#include <Veng/Application.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Localization/LocaleIndex.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Platform/UserPaths.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Settings/SettingsSchema.h>
#include <Veng/Settings/SettingsStore.h>

using namespace Veng;

namespace
{
    using TestSupport::BootstrapAsset;
    using TestSupport::PushPod;
    using TestSupport::WriteBootstrapFixture;

    // The app name keying the per-user config directory. Distinct from every shipped app and every
    // other test's, so the settings files this case plants are its own and its cleanup destroys
    // nothing else.
    constexpr const char* AppName = "veng-application-settings-boot-test";

    constexpr AssetId GraphicsSchemaId{0x7F47FE35041E2798ULL};
    constexpr AssetId AudioSchemaId{0xB3369C1A4DAC1306ULL};
    constexpr AssetId LocaleIndexId{0x0496170C8285BBF8ULL};
    constexpr AssetId FrCatalogId{0x6CA71161464475F6ULL};

    // The values planted on disk. Each is off its domain's default, so reading it back can only mean
    // the engine loaded the file — a store left at defaults reports none of them.
    constexpr f32 StoredRenderScale = 0.5f;
    constexpr const char* StoredShadows = "off";
    constexpr const char* StoredMusic = "quiet";
    constexpr const char* StoredLanguage = "fr";
    constexpr const char* GreetingKey = "greeting";
    constexpr const char* FrenchGreeting = "Bonjour";

    // A one-setting schema per domain: enough for a stored choice to be bounded and read back, with
    // a default option deliberately not the one the file names.
    SettingsSchemaData OneSettingSchema(string settingId, string firstOption, string secondOption)
    {
        SettingsCategory category;
        category.Id = "general";
        category.Label = "General";
        category.Settings.push_back(
            SettingsSetting{.Id = std::move(settingId),
                            .Label = "Setting",
                            .Kind = SettingsSettingKind::Discrete,
                            .Options = {{.Id = std::move(firstOption), .Label = "First"},
                                        {.Id = std::move(secondOption), .Label = "Second"}},
                            .DefaultOption = 1});
        SettingsSchemaData schema;
        schema.Categories.push_back(std::move(category));
        return schema;
    }

    // Encodes a settings-schema blob as its loader decodes it: the fixed header plus the tolerant
    // reflection record the cook writes.
    vector<u8> EncodeSettingsSchema(const TypeRegistry& types, const SettingsSchemaData& schema)
    {
        vector<u8> record;
        WriteFields(record, &schema, types.Info(TypeIdOf<SettingsSchemaData>()), types);

        vector<u8> blob;
        PushPod(blob, CookedSettingsSchemaHeader{.Version = CookedSettingsSchemaVersion,
                                                 .RecordBytes = static_cast<u32>(record.size())});
        blob.insert(blob.end(), record.begin(), record.end());
        return blob;
    }

    void WriteFixed(char (&dst)[LocaleIdCapacity], const std::string_view text)
    {
        std::memcpy(dst, text.data(), text.size());
        dst[text.size()] = '\0';
    }

    // Encodes a single-message locale catalog, so the resolved service has a translation to return
    // and "the index-backed service is live" is checkable by what it resolves, not only by its shape.
    vector<u8> EncodeCatalog(const std::string_view locale, const std::string_view key,
                             const std::string_view message)
    {
        vector<u8> pool;
        const auto append = [&pool](const std::string_view text)
        {
            const CookedLocaleStringSpan span{.Offset = static_cast<u32>(pool.size()),
                                              .Length = static_cast<u32>(text.size())};
            pool.insert(pool.end(), text.begin(), text.end());
            return span;
        };

        CookedLocaleEntry entry;
        entry.KeyHash = HashLocaleKey(key);
        entry.Key = append(key);
        entry.Variants[static_cast<usize>(Localization::PluralCategory::Other)] = append(message);
        entry.PresentMask = 1u << static_cast<u32>(Localization::PluralCategory::Other);

        CookedLocaleCatalogHeader header;
        header.Magic = CookedLocaleCatalogMagic;
        header.Version = CookedLocaleCatalogVersion;
        WriteFixed(header.LocaleId, locale);
        WriteFixed(header.FallbackId, locale);
        WriteFixed(header.PluralRule, locale);
        header.Decimal = static_cast<u32>(U'.');
        header.Grouping = static_cast<u32>(U',');
        header.EntryCount = 1;
        header.StringPoolBytes = static_cast<u32>(pool.size());

        vector<u8> blob;
        PushPod(blob, header);
        PushPod(blob, entry);
        blob.insert(blob.end(), pool.begin(), pool.end());
        return blob;
    }

    // Encodes a two-locale index whose source locale is *not* the one the stored language names, so
    // the active locale can only come from the file the boot read.
    vector<u8> EncodeLocaleIndex()
    {
        vector<u8> pool;
        const auto append = [&pool](const std::string_view text)
        {
            const CookedLocaleStringSpan span{.Offset = static_cast<u32>(pool.size()),
                                              .Length = static_cast<u32>(text.size())};
            pool.insert(pool.end(), text.begin(), text.end());
            return span;
        };

        // "en" is the source locale and ships no catalog of its own — a key it would have to resolve
        // is not asked for; "fr" is the stored choice and carries the one message the case reads.
        CookedLocaleIndexEntry en;
        WriteFixed(en.LocaleId, "en");
        WriteFixed(en.FallbackId, "en");
        en.CatalogId = FrCatalogId.Value;
        en.DisplayName = append("English");

        CookedLocaleIndexEntry fr;
        WriteFixed(fr.LocaleId, "fr");
        WriteFixed(fr.FallbackId, "en");
        fr.CatalogId = FrCatalogId.Value;
        fr.DisplayName = append("Français");

        CookedLocaleIndexHeader header;
        header.Magic = CookedLocaleIndexMagic;
        header.Version = CookedLocaleIndexVersion;
        WriteFixed(header.SourceLocaleId, "en");
        header.LocaleCount = 2;
        header.StringPoolBytes = static_cast<u32>(pool.size());

        vector<u8> blob;
        PushPod(blob, header);
        PushPod(blob, en);
        PushPod(blob, fr);
        blob.insert(blob.end(), pool.begin(), pool.end());
        return blob;
    }

    // Everything OnInitialize saw, read out of the run by the assertions below. Every field is the
    // answer to "had the boot done this yet?", so an unset one means the store was not there.
    struct BootSnapshot
    {
        bool Initialized = false;
        bool GraphicsLoadedFromFile = false;
        f32 GraphicsRenderScale = 0.0f;
        string GraphicsShadows;
        bool AudioStorePresent = false;
        bool AudioLoadedFromFile = false;
        string AudioMusic;
        bool LanguageStorePresent = false;
        string LanguageChoice;
        string ActiveLocale;
        usize AvailableLocales = 0;
        string Greeting;
    };

    // A headless managed-world Application that snapshots the three settings stores and the
    // localization service from inside OnInitialize, then quits on its first frame.
    class SettingsBootApp final : public Application
    {
    public:
        SettingsBootApp(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems,
                        BootSnapshot& snapshot)
            : Application(std::move(info), types, systems), m_Snapshot(snapshot)
        {
        }

    protected:
        void OnInitialize() override
        {
            m_Snapshot.Initialized = true;

            const GraphicsSettings& graphics = GetGraphicsSettings();
            m_Snapshot.GraphicsLoadedFromFile = graphics.WasLoadedFromFile();
            m_Snapshot.GraphicsRenderScale = graphics.GetDisplay().RenderScale;
            m_Snapshot.GraphicsShadows = graphics.GetChosenOption("shadows");

            if (const SettingsStore<SettingsChoices>* audio = GetAudioSettings())
            {
                m_Snapshot.AudioStorePresent = true;
                m_Snapshot.AudioLoadedFromFile = audio->WasLoadedFromFile();
                m_Snapshot.AudioMusic = audio->GetChosenOption("music");
            }

            if (const SettingsStore<SettingsChoices>* language = GetLanguageSettings())
            {
                m_Snapshot.LanguageStorePresent = true;
                m_Snapshot.LanguageChoice = language->GetChosenOption("language");
            }

            const Localization::Localization& localization = GetLocalization();
            m_Snapshot.ActiveLocale = string(localization.ActiveLocale());
            m_Snapshot.AvailableLocales = localization.AvailableLocales().size();
            m_Snapshot.Greeting = string(localization.Get(GreetingKey));
        }

        void OnUpdate(f32) override { RequestExit(); }

    private:
        BootSnapshot& m_Snapshot;
    };

    // Plants one settings file per domain under the app's config directory, through the very stores
    // the engine loads them with. Returns the directory so the case can remove it afterwards.
    path PlantStoredSettings(const TypeRegistry& types, const SettingsSchema& graphicsSchema,
                             const SettingsSchema& audioSchema)
    {
        const Result<path> configDir = UserConfigDir(AppName);
        REQUIRE(configDir.has_value());

        {
            GraphicsSettings graphics(
                GraphicsSettingsInfo{.Schema = &graphicsSchema,
                                     .Types = &types,
                                     .ConfigPath = *configDir / "graphics.json"});
            graphics.GetDisplay().RenderScale = StoredRenderScale;
            graphics.SetChosenOption("shadows", StoredShadows);
            REQUIRE(graphics.Save().has_value());
        }
        {
            SettingsStore<SettingsChoices> audio(SettingsStoreInfo{
                .Schema = &audioSchema, .Types = &types, .ConfigPath = *configDir / "audio.json"});
            audio.SetChosenOption("music", StoredMusic);
            REQUIRE(audio.Save().has_value());
        }
        {
            SettingsStore<SettingsChoices> language(SettingsStoreInfo{
                .Schema = nullptr, .Types = &types, .ConfigPath = *configDir / "locale.json"});
            language.SetChosenOption("language", StoredLanguage);
            REQUIRE(language.Save().has_value());
        }
        return *configDir;
    }
}

TEST_CASE("OnInitialize sees all three settings stores loaded and the index-backed localization")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    const SettingsSchemaData graphicsSchemaData = OneSettingSchema("shadows", "off", "high");
    const SettingsSchemaData audioSchemaData = OneSettingSchema("music", "quiet", "loud");
    const Ref<SettingsSchema> graphicsSchema = SettingsSchema::Create(graphicsSchemaData);
    const Ref<SettingsSchema> audioSchema = SettingsSchema::Create(audioSchemaData);

    const path configDir = PlantStoredSettings(types, *graphicsSchema, *audioSchema);

    const vector<BootstrapAsset> extra{
        BootstrapAsset{.Id = GraphicsSchemaId,
                       .Type = AssetTypes::SettingsSchema,
                       .Blob = EncodeSettingsSchema(types, graphicsSchemaData)},
        BootstrapAsset{.Id = AudioSchemaId,
                       .Type = AssetTypes::SettingsSchema,
                       .Blob = EncodeSettingsSchema(types, audioSchemaData)},
        BootstrapAsset{
            .Id = LocaleIndexId, .Type = AssetTypes::LocaleIndex, .Blob = EncodeLocaleIndex()},
        BootstrapAsset{.Id = FrCatalogId,
                       .Type = AssetTypes::LocaleCatalog,
                       .Blob = EncodeCatalog("fr", GreetingKey, FrenchGreeting)},
    };
    const path project = WriteBootstrapFixture(types, "settings_boot", extra);

    BootSnapshot snapshot;

    ApplicationInfo info;
    info.Name = AppName;
    info.Headless = true;
    info.ImGui = std::nullopt;
    info.ManagedViewport = ManagedViewportInfo{};
    info.World = GameWorldInfo{.Project = project, .RestoreLocalSessionOnBoot = false};
    info.GraphicsSchema = GraphicsSchemaId;
    info.AudioSettingsSchema = AudioSchemaId;
    info.LocaleIndex = LocaleIndexId;

    {
        SettingsBootApp app(std::move(info), types, systems, snapshot);
        app.Run({});
    }

    REQUIRE(snapshot.Initialized);

    // The graphics store: built *and* loaded before the hook, so the first-run signal and the stored
    // values are both readable there — this is what lets a consumer act before the first frame.
    CHECK(snapshot.GraphicsLoadedFromFile);
    CHECK(snapshot.GraphicsRenderScale == doctest::Approx(StoredRenderScale));
    CHECK(snapshot.GraphicsShadows == StoredShadows);

    // The audio store: present because a schema was named, and loaded — its schema default is the
    // other option, so the stored one can only have come off disk.
    CHECK(snapshot.AudioStorePresent);
    CHECK(snapshot.AudioLoadedFromFile);
    CHECK(snapshot.AudioMusic == StoredMusic);

    // The language store and the service built on it: the active locale is the stored choice, not
    // the index's source locale, and it resolves through the chosen locale's catalog.
    CHECK(snapshot.LanguageStorePresent);
    CHECK(snapshot.LanguageChoice == StoredLanguage);
    CHECK(snapshot.ActiveLocale == StoredLanguage);
    CHECK(snapshot.AvailableLocales == 2);
    CHECK(snapshot.Greeting == FrenchGreeting);

    std::filesystem::remove_all(configDir);
}
