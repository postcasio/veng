#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Localization/Message.h>

namespace Veng
{
    class LocaleCatalogLoader;
}

namespace Veng::Localization
{
    /// @brief One decoded message in a locale catalog: its key, the key's hash, and the message.
    ///
    /// Entries are held sorted ascending by KeyHash (ties broken by Key), so a lookup is a binary
    /// search over the hashes with a key-string compare disambiguating a hash collision.
    struct LocaleCatalogEntry
    {
        /// @brief The message key's stable hash (HashLocaleKey), the sort/search key.
        u64 KeyHash = 0;
        /// @brief The message key (owned), for exact comparison on a hash hit.
        string Key;
        /// @brief The decoded message: a single template or a set of plural variants.
        Message Message;
    };

    /// @brief One locale's message catalog: a sorted key→message table with locale metadata.
    ///
    /// Cooked from a `*.loc.json` source (AssetTypes::LocaleCatalog), loaded by id through the
    /// ordinary AssetManager path — a CPU-only asset with no GPU resource. It owns its decoded
    /// strings, so a returned string_view aliases storage that lives as long as the catalog. The
    /// localization service loads a locale's catalog plus its fallback chain and resolves a key by
    /// FindMessage; the catalog itself performs no fallback (it holds one locale) — the chain is the
    /// service's job.
    class LocaleCatalog
    {
    public:
        /// @brief The decoded parts of a cooked catalog blob, as the loader hands them over.
        struct Contents
        {
            /// @brief This catalog's locale id.
            string LocaleId;
            /// @brief The fallback locale id (equals LocaleId for a terminal locale).
            string FallbackId;
            /// @brief The CLDR plural-rule selector (a locale id resolved through PluralRuleFor).
            string PluralRuleId;
            /// @brief The locale's number separators.
            NumberFormat Numbers;
            /// @brief The locale's elision table; empty for a locale that elides nothing.
            ElisionTable Elision;
            /// @brief The message entries, sorted ascending by KeyHash then Key.
            vector<LocaleCatalogEntry> Entries;
        };

        /// @brief Creates a catalog from a decoded blob's contents.
        /// @param contents  The decoded parts; the loader has sorted Entries before this call.
        /// @return The constructed catalog.
        [[nodiscard]] static Ref<LocaleCatalog> Create(Contents contents);

        /// @brief Finds the message for a key, or nullptr when this catalog does not define it.
        ///
        /// A binary search over the key hashes, then an exact key compare over the equal-hash run so
        /// a hash collision never returns the wrong message.
        /// @param key  The message key.
        /// @return The message, or nullptr when absent.
        [[nodiscard]] const Message* FindMessage(std::string_view key) const;

        /// @brief Returns this catalog's locale id.
        [[nodiscard]] std::string_view GetLocaleId() const { return m_LocaleId; }

        /// @brief Returns the fallback locale id (equal to the locale id for a terminal locale).
        [[nodiscard]] std::string_view GetFallbackId() const { return m_FallbackId; }

        /// @brief Returns the CLDR plural-rule selector for this locale.
        [[nodiscard]] std::string_view GetPluralRuleId() const { return m_PluralRuleId; }

        /// @brief Returns the locale's number separators.
        [[nodiscard]] NumberFormat GetNumbers() const { return m_Numbers; }

        /// @brief Returns the locale's elision table (empty when it elides nothing).
        [[nodiscard]] const ElisionTable& GetElision() const { return m_Elision; }

        /// @brief Returns the number of message entries.
        [[nodiscard]] usize GetEntryCount() const { return m_Entries.size(); }

    private:
        explicit LocaleCatalog(Contents contents);

        /// @brief This catalog's locale id.
        string m_LocaleId;
        /// @brief The fallback locale id.
        string m_FallbackId;
        /// @brief The CLDR plural-rule selector.
        string m_PluralRuleId;
        /// @brief The locale's number separators.
        NumberFormat m_Numbers;
        /// @brief The locale's elision table.
        ElisionTable m_Elision;
        /// @brief The message entries, sorted ascending by KeyHash then Key.
        vector<LocaleCatalogEntry> m_Entries;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping LocaleCatalog to AssetTypes::LocaleCatalog.
    template <>
    struct AssetTypeTrait<Localization::LocaleCatalog>
    {
        /// @brief The asset type tag for LocaleCatalog.
        static constexpr AssetTypeId Type = AssetTypes::LocaleCatalog;
    };
}
