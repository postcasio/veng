#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

namespace Veng::Localization
{
    /// @brief One locale a project ships: its id, endonym, catalog, and fallback.
    struct LocaleEntry
    {
        /// @brief The locale id (a BCP-47-ish tag, e.g. "en", "pt-BR").
        string Id;
        /// @brief The endonym — the locale's display name in its own language.
        string DisplayName;
        /// @brief AssetId of this locale's LocaleCatalog.
        AssetId Catalog;
        /// @brief The fallback locale id (equals Id for a terminal/source locale).
        string Fallback;
    };

    /// @brief One project's index of the locales it ships: the available locales and the source one.
    ///
    /// Cooked from a `*.locindex.json` source (AssetTypes::LocaleIndex), boot-loaded by id. It names
    /// each locale's catalog by id and its fallback, plus the source locale keys are authored in
    /// (the terminus of every fallback chain). The cook validates the fallback graph, so a loaded
    /// index is acyclic and every fallback names a present locale; the runtime service still guards
    /// its walk as defence.
    class LocaleIndex
    {
    public:
        /// @brief The decoded parts of a cooked index blob, as the loader hands them over.
        struct Contents
        {
            /// @brief The source locale id (the fallback terminus).
            string SourceLocale;
            /// @brief The available locales, in authored order.
            vector<LocaleEntry> Locales;
        };

        /// @brief Creates an index from a decoded blob's contents.
        /// @param contents  The decoded parts.
        /// @return The constructed index.
        [[nodiscard]] static Ref<LocaleIndex> Create(Contents contents);

        /// @brief Returns the source locale id (the fallback terminus).
        [[nodiscard]] std::string_view GetSourceLocale() const { return m_SourceLocale; }

        /// @brief Returns every available locale, in authored order.
        [[nodiscard]] const vector<LocaleEntry>& GetLocales() const { return m_Locales; }

        /// @brief Finds a locale by id, or nullptr when the index does not name it.
        /// @param id  The locale id to look up.
        /// @return The entry, or nullptr.
        [[nodiscard]] const LocaleEntry* Find(std::string_view id) const;

    private:
        explicit LocaleIndex(Contents contents);

        /// @brief The source locale id.
        string m_SourceLocale;
        /// @brief The available locales, in authored order.
        vector<LocaleEntry> m_Locales;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping LocaleIndex to AssetTypes::LocaleIndex.
    template <>
    struct AssetTypeTrait<Localization::LocaleIndex>
    {
        /// @brief The asset type tag for LocaleIndex.
        static constexpr AssetTypeId Type = AssetTypes::LocaleIndex;
    };
}
