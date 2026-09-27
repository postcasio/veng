// Localization service resolution: an active-locale hit, a fallback-chain hit, a missing key
// resolving to itself, a SetLocale swap bumping the generation and changing what Get returns, and
// Format selecting the active locale's plural rule, number separators and elision table. The
// catalog blobs are assembled by hand here — through the same layout and key hash the cook writes —
// and mounted over an in-memory archive, so the runtime service is tested independently of the
// cooker. The index is built directly (LocaleIndex::Create), since the index blob round-trip is a
// cooker-side test.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Localization/LocaleCatalog.h>
#include <Veng/Localization/LocaleIndex.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    constexpr AssetId EnCatalogId{0x10CA1E0000000001ULL};
    constexpr AssetId FrCatalogId{0x10CA1E0000000002ULL};

    // One authored message for the test encoder: a key plus its variant strings (Other for a plain
    // template, or several for a plural message). An empty optional means the variant is absent.
    struct TestMessage
    {
        string Key;
        bool IsPlural = false;
        std::array<optional<string>, CookedLocalePluralCategoryCount> Variants;
    };

    TestMessage Plain(string key, string value)
    {
        TestMessage message{.Key = std::move(key)};
        message.Variants[static_cast<usize>(Localization::PluralCategory::Other)] =
            std::move(value);
        return message;
    }

    TestMessage Plural(string key, string one, string other)
    {
        TestMessage message{.Key = std::move(key), .IsPlural = true};
        message.Variants[static_cast<usize>(Localization::PluralCategory::One)] = std::move(one);
        message.Variants[static_cast<usize>(Localization::PluralCategory::Other)] =
            std::move(other);
        return message;
    }

    void WriteFixed(char (&dst)[LocaleIdCapacity], std::string_view text)
    {
        std::memcpy(dst, text.data(), text.size());
        dst[text.size()] = '\0';
    }

    template <class T>
    void Append(vector<u8>& out, const T& value)
    {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    // Encodes a catalog blob byte-for-byte as the cooker does, so the runtime loader decodes it.
    vector<u8> EncodeCatalog(string locale, string fallback, string pluralRule, char32_t decimal,
                             char32_t grouping, const vector<TestMessage>& messages,
                             const Localization::ElisionTable& elision = {})
    {
        struct Built
        {
            CookedLocaleEntry Entry;
            string Key;
        };
        vector<u8> pool;
        vector<Built> built;
        const auto append = [&](std::string_view text)
        {
            const CookedLocaleStringSpan span{.Offset = static_cast<u32>(pool.size()),
                                              .Length = static_cast<u32>(text.size())};
            pool.insert(pool.end(), text.begin(), text.end());
            return span;
        };

        for (const TestMessage& message : messages)
        {
            CookedLocaleEntry entry;
            entry.KeyHash = HashLocaleKey(message.Key);
            entry.Key = append(message.Key);
            entry.IsPlural = message.IsPlural ? 1u : 0u;
            entry.PresentMask = 0;
            for (usize i = 0; i < CookedLocalePluralCategoryCount; ++i)
            {
                if (message.Variants[i])
                {
                    entry.Variants[i] = append(*message.Variants[i]);
                    entry.PresentMask |= (1u << i);
                }
            }
            built.push_back({.Entry = entry, .Key = message.Key});
        }
        std::ranges::sort(built, [](const Built& a, const Built& b)
                          { return a.Entry.KeyHash < b.Entry.KeyHash; });

        CookedLocaleCatalogHeader header;
        header.Magic = CookedLocaleCatalogMagic;
        header.Version = CookedLocaleCatalogVersion;
        WriteFixed(header.LocaleId, locale);
        WriteFixed(header.FallbackId, fallback);
        WriteFixed(header.PluralRule, pluralRule);
        header.Decimal = static_cast<u32>(decimal);
        header.Grouping = static_cast<u32>(grouping);
        header.EntryCount = static_cast<u32>(built.size());
        header.ElisionInitials = append(elision.Initials);
        vector<CookedLocaleElisionRule> rules;
        for (const Localization::ElisionRule& rule : elision.Rules)
        {
            rules.push_back({.Word = append(rule.Word), .Elided = append(rule.Elided)});
        }
        header.ElisionRuleCount = static_cast<u32>(rules.size());
        header.StringPoolBytes = static_cast<u32>(pool.size());

        vector<u8> blob;
        Append(blob, header);
        for (const Built& entry : built)
        {
            Append(blob, entry.Entry);
        }
        for (const CookedLocaleElisionRule& rule : rules)
        {
            Append(blob, rule);
        }
        blob.insert(blob.end(), pool.begin(), pool.end());
        return blob;
    }

    struct Host
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Manager;

        Host() { Manager = CreateUnique<AssetManager>(Context, Tasks, Types); }

        void MountCatalogs()
        {
            ArchiveWriter writer;
            // en: source, terminal. Grouping ',' decimal '.'. Carries a key fr lacks (only_en).
            writer.Add(EnCatalogId, AssetTypes::LocaleCatalog,
                       EncodeCatalog("en", "en", "en", U'.', U',',
                                     {Plain("greeting", "Hello"), Plain("only_en", "English only"),
                                      Plural("count", "{#} item", "{#} items")}));
            // fr: falls back to en, decimal comma + space grouping, and elides "de" before a
            // vowel. Defines 'greeting' and 'possession'.
            writer.Add(
                FrCatalogId, AssetTypes::LocaleCatalog,
                EncodeCatalog(
                    "fr", "en", "fr", U',', U' ',
                    {Plain("greeting", "Bonjour"), Plain("possession", "le livre de {name}")},
                    {.Initials = "aeiouéè", .Rules = {{.Word = "de", .Elided = "d'"}}}));

            const path archive =
                Veng::TestSupport::TempDir() / "veng_localization_service.vengpack";
            REQUIRE(writer.Write(archive).has_value());
            REQUIRE(Manager->Mount(archive).has_value());
        }
    };

    Ref<Localization::LocaleIndex> BuildIndex()
    {
        return Localization::LocaleIndex::Create(Localization::LocaleIndex::Contents{
            .SourceLocale = "en",
            .Locales = {Localization::LocaleEntry{.Id = "en",
                                                  .DisplayName = "English",
                                                  .Catalog = EnCatalogId,
                                                  .Fallback = "en"},
                        Localization::LocaleEntry{.Id = "fr",
                                                  .DisplayName = "Français",
                                                  .Catalog = FrCatalogId,
                                                  .Fallback = "en"}},
        });
    }
}

TEST_CASE("Localization: active hit, fallback hit, and missing key resolve correctly")
{
    Host host;
    host.MountCatalogs();
    const Ref<Localization::LocaleIndex> index = BuildIndex();

    const Localization::Localization loc(*host.Manager, *index, "fr");

    CHECK(loc.ActiveLocale() == "fr");
    CHECK(loc.AvailableLocales().size() == 2);
    // Active (fr) defines 'greeting'.
    CHECK(loc.Get("greeting") == "Bonjour");
    // 'only_en' is not in fr, so the walk falls back to en.
    CHECK(loc.Get("only_en") == "English only");
    // A key no catalog defines resolves to itself, never a blank.
    CHECK(loc.Get("does.not.exist") == "does.not.exist");
}

TEST_CASE("Localization: SetLocale bumps the generation and changes resolution")
{
    Host host;
    host.MountCatalogs();
    const Ref<Localization::LocaleIndex> index = BuildIndex();

    Localization::Localization loc(*host.Manager, *index, "fr");
    const u32 before = loc.Generation();
    CHECK(loc.Get("greeting") == "Bonjour");

    loc.SetLocale("en");
    CHECK(loc.Generation() == before + 1);
    CHECK(loc.ActiveLocale() == "en");
    CHECK(loc.Get("greeting") == "Hello");
}

TEST_CASE("Localization: Format selects the active plural rule and number separators")
{
    Host host;
    host.MountCatalogs();
    const Ref<Localization::LocaleIndex> index = BuildIndex();

    // Active en: English plural rule, '.'/',' separators.
    const Localization::Localization en(*host.Manager, *index, "en");
    CHECK(en.Format("count", {}, 1) == "1 item");
    CHECK(en.Format("count", {}, 5) == "5 items");
    CHECK(en.Format("count", {}, 12000) == "12,000 items");

    // Active fr: the 'count' message falls back to en, but the plural rule and number separators
    // come from the active locale (fr): a space grouping and the French rule (0/1 → one).
    const Localization::Localization fr(*host.Manager, *index, "fr");
    CHECK(fr.Numbers().Grouping == U' ');
    CHECK(fr.Format("count", {}, 1) == "1 item");
    CHECK(fr.Format("count", {}, 12000) == "12 000 items");
}

TEST_CASE("Localization: Format applies the active locale's elision table")
{
    Host host;
    host.MountCatalogs();
    const Ref<Localization::LocaleIndex> index = BuildIndex();

    const auto args = [](std::string_view name)
    { return std::array<Localization::FormatArg, 1>{{{.Name = "name", .Value = name}}}; };

    const Localization::Localization fr(*host.Manager, *index, "fr");
    CHECK(fr.Elision().Rules.size() == 1);
    CHECK(fr.Format("possession", args("Émile")) == "le livre d'Émile");
    CHECK(fr.Format("possession", args("Paul")) == "le livre de Paul");
    CHECK(fr.Format("possession", args("ABC 12")) == "le livre de ABC 12");

    // en authors no table, so it elides nothing.
    const Localization::Localization en(*host.Manager, *index, "en");
    CHECK(en.Elision().Empty());
}

TEST_CASE("Localization: the null-object resolves every key to itself")
{
    Localization::Localization null;
    CHECK(null.Get("menu.play") == "menu.play");
    CHECK(null.Format("menu.play") == "menu.play");
    CHECK(null.AvailableLocales().empty());
    CHECK(null.Generation() == 0);
    // SetLocale is a no-op on the null-object (no index to switch within).
    null.SetLocale("fr");
    CHECK(null.Generation() == 0);

    // The shared instance a seam handing out a service *reference* falls back to behaves the same,
    // so a consumer wired no service reads keys rather than needing a null check.
    CHECK(Localization::NullService().Get("menu.play") == "menu.play");
    CHECK(Localization::NullService().AvailableLocales().empty());
}
