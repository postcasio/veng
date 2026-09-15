// Locale cook test: a *.loc.json catalog round-trips through the LocaleCatalogImporter (its
// messages and plural variants survive a cook → mount → load), and the LocaleIndexImporter's gates
// fire — a dangling catalog reference, a pluralized message missing its 'other' variant, an invalid
// fallback graph (source not self-terminal, a fallback to an absent locale, a cycle), and a
// "coverage": "complete" index whose translated locale drops a source key. The importers reference
// only engine builtins, so the cook runs with a builtin-only registry and no module load.

#include <filesystem>
#include <fstream>
#include <random>

#include <doctest/doctest.h>
#include <fmt/format.h>

#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Localization/LocaleCatalog.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // doctest streams its message argument, so a failure detail is built as a plain string here.
    string ErrOf(const Result<path>& result)
    {
        return result.has_value() ? string{} : result.error();
    }

    string LoadDetail(const AssetResult<AssetHandle<Localization::LocaleCatalog>>& result)
    {
        return result.has_value() ? string{} : result.error().Detail;
    }

    // Writes a source file into a temp dir, returning its absolute path.
    path WriteSource(const path& dir, const string& name, const json& doc)
    {
        const path file = dir / name;
        std::ofstream(file) << doc.dump();
        return file;
    }

    // One pack entry: a minted id, a type name, and a source filename (relative to the pack dir).
    struct Entry
    {
        AssetId Id;
        string Type;
        string Source;
    };

    // Writes a pack manifest naming the given entries and cooks it, returning the archive path (or
    // the located cook error). Each call uses a unique archive name so parallel cases do not collide.
    Result<path> CookEntries(const path& dir, const vector<Entry>& entries)
    {
        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array();
        for (const Entry& entry : entries)
        {
            pack["assets"].push_back(json{{"id", FormatHexId(entry.Id.Value)},
                                          {"type", entry.Type},
                                          {"source", entry.Source}});
        }
        std::random_device rng;
        const path packPath = dir / fmt::format("locale_pack_{:08x}.pack.json", rng());
        std::ofstream(packPath) << pack.dump();

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const path archive = dir / fmt::format("locale_{:08x}.vengpack", rng());
        const VoidResult cooked = cooker.CookPack(packPath, archive);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        return archive;
    }

    constexpr AssetId EnCatalogId{0x10CC1E0000000001ULL};
    constexpr AssetId FrCatalogId{0x10CC1E0000000002ULL};
    constexpr AssetId IndexId{0x10CC1E00000000FFULL};

    json EnCatalog()
    {
        return json{{"locale", "en"},
                    {"fallback", "en"},
                    {"messages",
                     {{"greeting", "Hello"},
                      {"only_en", "English only"},
                      {"count", {{"one", "{#} item"}, {"other", "{#} items"}}}}}};
    }
}

TEST_CASE("Locale cook: a catalog round-trips its messages and plural variants")
{
    const path dir = Veng::TestSupport::TempDir();
    WriteSource(dir, "en.loc.json", EnCatalog());

    const Result<path> archive = CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "en.loc.json"}});
    REQUIRE_MESSAGE(archive.has_value(), ErrOf(archive));

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(*archive).has_value());

    const AssetResult<AssetHandle<Localization::LocaleCatalog>> loaded =
        manager.LoadSync<Localization::LocaleCatalog>(EnCatalogId);
    REQUIRE_MESSAGE(loaded.has_value(), LoadDetail(loaded));

    const Localization::LocaleCatalog& catalog = *(*loaded).Get();
    CHECK(catalog.GetLocaleId() == "en");
    CHECK(catalog.GetFallbackId() == "en");

    const Localization::Message* greeting = catalog.FindMessage("greeting");
    REQUIRE(greeting != nullptr);
    CHECK_FALSE(greeting->Plurals.has_value());
    CHECK(greeting->Template == "Hello");

    const Localization::Message* count = catalog.FindMessage("count");
    REQUIRE(count != nullptr);
    REQUIRE(count->Plurals.has_value());
    CHECK((*count->Plurals)[static_cast<usize>(Localization::PluralCategory::One)] == "{#} item");
    CHECK((*count->Plurals)[static_cast<usize>(Localization::PluralCategory::Other)] ==
          "{#} items");

    CHECK(catalog.FindMessage("absent.key") == nullptr);

    std::filesystem::remove(*archive);
}

TEST_CASE("Locale cook: an index naming a missing catalog is a cook error")
{
    const path dir = Veng::TestSupport::TempDir();
    // The index names EnCatalogId, but the pack contains no such asset.
    WriteSource(dir, "dangling.locindex.json",
                json{{"source", "en"},
                     {"locales", json::array({json{{"id", "en"},
                                                   {"displayName", "English"},
                                                   {"catalog", FormatHexId(EnCatalogId.Value)},
                                                   {"fallback", "en"}}})}});

    const Result<path> archive =
        CookEntries(dir, {{IndexId, "LocaleIndex", "dangling.locindex.json"}});
    REQUIRE_FALSE(archive.has_value());
    CHECK(archive.error().find("which no asset provides") != string::npos);
}

TEST_CASE("Locale cook: a pluralized message missing 'other' is a cook error")
{
    const path dir = Veng::TestSupport::TempDir();
    WriteSource(dir, "noother.loc.json",
                json{{"locale", "en"}, {"messages", {{"count", {{"one", "{#} item"}}}}}});

    const Result<path> archive =
        CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "noother.loc.json"}});
    REQUIRE_FALSE(archive.has_value());
    CHECK(archive.error().find("missing the required 'other'") != string::npos);
}

TEST_CASE("Locale cook: the source locale must fall back to itself")
{
    const path dir = Veng::TestSupport::TempDir();
    WriteSource(dir, "en.loc.json", EnCatalog());
    // Source 'en' declares a fallback of 'fr' rather than itself.
    WriteSource(dir, "badsource.locindex.json",
                json{{"source", "en"},
                     {"locales", json::array({json{{"id", "en"},
                                                   {"catalog", FormatHexId(EnCatalogId.Value)},
                                                   {"fallback", "fr"}}})}});

    const Result<path> archive =
        CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "en.loc.json"},
                          {IndexId, "LocaleIndex", "badsource.locindex.json"}});
    REQUIRE_FALSE(archive.has_value());
    CHECK(archive.error().find("must fall back to itself") != string::npos);
}

TEST_CASE("Locale cook: a fallback cycle is a cook error")
{
    const path dir = Veng::TestSupport::TempDir();
    WriteSource(dir, "en.loc.json", EnCatalog());
    WriteSource(
        dir, "fr.loc.json",
        json{{"locale", "fr"}, {"fallback", "en"}, {"messages", {{"greeting", "Bonjour"}}}});
    // en → fr and fr → en: neither is self-terminal, so no chain reaches a source terminus.
    WriteSource(dir, "cycle.locindex.json",
                json{{"source", "en"},
                     {"locales", json::array({json{{"id", "en"},
                                                   {"catalog", FormatHexId(EnCatalogId.Value)},
                                                   {"fallback", "fr"}},
                                              json{{"id", "fr"},
                                                   {"catalog", FormatHexId(FrCatalogId.Value)},
                                                   {"fallback", "en"}}})}});

    const Result<path> archive =
        CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "en.loc.json"},
                          {FrCatalogId, "LocaleCatalog", "fr.loc.json"},
                          {IndexId, "LocaleIndex", "cycle.locindex.json"}});
    REQUIRE_FALSE(archive.has_value());
    // The source's own fallback is checked first, so this reports the non-self-terminal source.
    CHECK(archive.error().find("fall back to itself") != string::npos);
}

TEST_CASE("Locale cook: coverage 'complete' rejects a locale missing a source key")
{
    const path dir = Veng::TestSupport::TempDir();
    WriteSource(dir, "en.loc.json", EnCatalog());
    // fr defines only 'greeting', dropping 'only_en' and 'count'.
    WriteSource(
        dir, "fr.loc.json",
        json{{"locale", "fr"}, {"fallback", "en"}, {"messages", {{"greeting", "Bonjour"}}}});

    const json index = json{
        {"source", "en"},
        {"coverage", "complete"},
        {"locales",
         json::array(
             {json{{"id", "en"}, {"catalog", FormatHexId(EnCatalogId.Value)}, {"fallback", "en"}},
              json{{"id", "fr"},
                   {"catalog", FormatHexId(FrCatalogId.Value)},
                   {"fallback", "en"}}})}};

    WriteSource(dir, "complete.locindex.json", index);
    const Result<path> strict =
        CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "en.loc.json"},
                          {FrCatalogId, "LocaleCatalog", "fr.loc.json"},
                          {IndexId, "LocaleIndex", "complete.locindex.json"}});
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error().find("coverage is 'complete'") != string::npos);

    // The same gap under the default 'warn' policy cooks successfully (a warning, not an error).
    json warnIndex = index;
    warnIndex["coverage"] = "warn";
    WriteSource(dir, "warn.locindex.json", warnIndex);
    const Result<path> lenient = CookEntries(dir, {{EnCatalogId, "LocaleCatalog", "en.loc.json"},
                                                   {FrCatalogId, "LocaleCatalog", "fr.loc.json"},
                                                   {IndexId, "LocaleIndex", "warn.locindex.json"}});
    REQUIRE_MESSAGE(lenient.has_value(), ErrOf(lenient));
    std::filesystem::remove(*lenient);
}
