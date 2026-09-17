#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Localization/LocaleCatalog.h>
#include <Veng/Localization/LocaleIndex.h>
#include <Veng/Localization/Message.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Localization
{
    /// @brief The engine-owned localization service: resolves a key to a message in the active locale.
    ///
    /// The AssetManager cache is keyed by AssetId alone, so it cannot select a locale's catalog by a
    /// runtime language choice — that resolution lives here. Constructed over a loaded LocaleIndex
    /// and a chosen locale, the service LoadSyncs the active locale's LocaleCatalog and its fallback
    /// chain (walking the index's fallback edges with a visited-set), and resolves a key through
    /// active → fallback chain → the key itself, so a missing translation is a visible, debuggable
    /// string and never a blank. A language change reloads the chain and bumps a generation counter
    /// a consumer compares to know when to re-resolve.
    ///
    /// Application always owns one: a real, index-backed service when ApplicationInfo::LocaleIndex is
    /// set, else an inert null-object (default-constructed) that resolves every key to itself, so
    /// SystemContext and every consumer always have a referent. It names no renderer or Gui type — a
    /// string service over the asset manager. Like AudioEngine it is main-thread-only: SetLocale
    /// mutates the chain and generation and Get/Format read them, all on the main thread, so no
    /// synchronization is provided and no consumer calls it from a TaskSystem worker.
    class Localization
    {
    public:
        /// @brief Constructs the inert null-object: no index, every key resolves to itself.
        Localization() = default;

        /// @brief Constructs the index-backed service on a chosen locale.
        ///
        /// LoadSyncs the chosen locale's catalog and its fallback chain. A chosen locale the index
        /// does not name falls back to the index's source locale.
        /// @param assets  The asset manager the catalogs load through; must outlive the service.
        /// @param index   The loaded locale index; its data is copied, so it need not outlive this.
        /// @param chosenLocale  The initial active locale id.
        Localization(AssetManager& assets, const LocaleIndex& index, std::string_view chosenLocale);

        /// @brief Resolves a key to its message string in the active locale, then the fallback chain.
        ///
        /// Returns the message's raw template (the Other variant for a pluralized message); a caller
        /// wanting placeholder substitution or plural selection uses Format. A key no catalog in the
        /// chain defines resolves to the key itself.
        /// @param key  The message key.
        /// @return The resolved string, or the key when it is undefined; never empty for a non-empty key.
        [[nodiscard]] std::string_view Get(std::string_view key) const;

        /// @brief Formats a key's message against named arguments and a plural count.
        ///
        /// Resolves the message through the same chain as Get and hands it to FormatMessage with the
        /// active locale's plural rule and number separators. A missing key formats the key string
        /// itself; a format error (a malformed template or a bad spec) degrades to the raw template
        /// with its {name} markers left literal and logs once — never a crash and never a blank.
        /// @param key    The message key.
        /// @param args   The named arguments bound to the template's fields.
        /// @param count  The plural-selecting count, or nullopt for a non-count message.
        /// @return The formatted string.
        [[nodiscard]] string Format(std::string_view key, std::span<const FormatArg> args = {},
                                    optional<i64> count = std::nullopt) const;

        /// @brief Sets the active locale, reloading its catalog chain and bumping the generation.
        ///
        /// A no-op on the null-object. A locale the index does not name falls back to the source.
        /// @param id  The locale id to switch to.
        void SetLocale(std::string_view id);

        /// @brief Returns the locales the index names, in authored order (empty for the null-object).
        [[nodiscard]] std::span<const LocaleEntry> AvailableLocales() const { return m_Locales; }

        /// @brief Returns the active locale id (empty for the null-object).
        [[nodiscard]] std::string_view ActiveLocale() const { return m_ActiveLocale; }

        /// @brief Returns the generation counter, bumped on every SetLocale.
        [[nodiscard]] u32 Generation() const { return m_Generation; }

        /// @brief Returns the active locale's number separators.
        ///
        /// A game's own formatters read it so they render decimal/grouping separators consistently
        /// with Format. The English default for the null-object or a locale with no catalog.
        [[nodiscard]] NumberFormat Numbers() const { return m_Numbers; }

    private:
        /// @brief Finds a locale entry by id in the copied index, or nullptr.
        [[nodiscard]] const LocaleEntry* FindLocale(std::string_view id) const;

        /// @brief Loads the active locale's catalog chain, walking fallback edges with a visited-set.
        void BuildChain(std::string_view activeLocale);

        /// @brief Finds a key's message across the active catalog then the fallback chain, or nullptr.
        [[nodiscard]] const Message* Resolve(std::string_view key) const;

        /// @brief The asset manager the catalogs load through; null for the null-object.
        AssetManager* m_Assets = nullptr;
        /// @brief The index's locales, copied so the service is self-contained.
        vector<LocaleEntry> m_Locales;
        /// @brief The index's source locale id (the fallback terminus).
        string m_SourceLocale;
        /// @brief The active locale id.
        string m_ActiveLocale;
        /// @brief The active-then-fallback catalog chain, active first.
        vector<AssetHandle<LocaleCatalog>> m_Chain;
        /// @brief The active locale's plural rule (English for the null-object or an empty selector).
        PluralRule m_PluralRule = nullptr;
        /// @brief The active locale's number separators.
        NumberFormat m_Numbers;
        /// @brief Bumped on every SetLocale so a consumer can re-resolve on a language change.
        u32 m_Generation = 0;
        /// @brief Whether a format error has already been logged (the log-once gate).
        mutable bool m_FormatErrorLogged = false;
    };

    /// @brief The shared inert service a consumer handed none resolves against.
    ///
    /// A default-constructed Localization with static storage: every key resolves to itself, so a
    /// seam that must hand its consumer a service *reference* — a GuiDriverContext, a
    /// GuiDriverFrame — always has a referent, and a host that wired none (an editor preview, a
    /// device-free test) degrades to visible keys rather than a null check at every call site.
    /// Immutable in practice: nothing may SetLocale on it, because it is const.
    /// @return The process-wide inert service.
    [[nodiscard]] const Localization& NullService();
}
