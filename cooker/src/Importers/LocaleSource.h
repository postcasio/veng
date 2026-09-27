#pragma once

#include <array>
#include <string_view>

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <nlohmann/json.hpp>

namespace Veng::Cook
{
    using json = nlohmann::json;

    /// @brief One parsed message from a `*.loc.json` catalog source: its key and its variants.
    ///
    /// A non-pluralized message (IsPlural false) carries its sole template in the Other slot; a
    /// pluralized message (IsPlural true) carries one string per CLDR category it defines. The slot
    /// indices match the runtime PluralCategory ordinal (Zero..Other).
    struct ParsedLocaleMessage
    {
        /// @brief The message key (a dotted namespaced id, opaque to the cook).
        string Key;
        /// @brief True when the message is a plural-variant set; false for a single template.
        bool IsPlural = false;
        /// @brief Per-category variant strings, indexed by PluralCategory ordinal; unset when absent.
        std::array<optional<string>, CookedLocalePluralCategoryCount> Variants;
    };

    /// @brief One parsed rule of a catalog source's `elision.words` object: a word and its
    ///        elided form.
    struct ParsedElisionRule
    {
        /// @brief The word that elides (`"de"`).
        string Word;
        /// @brief The form replacing the word and the space after it (`"d'"`).
        string Elided;
    };

    /// @brief A parsed `*.loc.json` catalog source: locale metadata plus its messages.
    ///
    /// The optional `"elision"` object is the locale's elision table (Localization::ElisionTable
    /// holds the runtime rule): `"initials"`, a string of the letters that make a value begin with
    /// a vowel sound, and `"words"`, an object mapping each eliding word to its elided form —
    /// `{"initials": "aeiouéè", "words": {"de": "d'", "le": "l'"}}`.
    struct ParsedLocaleCatalog
    {
        /// @brief This catalog's locale id.
        string LocaleId;
        /// @brief The fallback locale id (defaults to LocaleId — a terminal locale).
        string FallbackId;
        /// @brief The CLDR plural-rule selector (defaults to LocaleId).
        string PluralRuleId;
        /// @brief The decimal separator codepoint.
        char32_t Decimal = U'.';
        /// @brief The grouping separator codepoint (U'\0' disables grouping).
        char32_t Grouping = U',';
        /// @brief The elision table's initial letters, UTF-8; empty when the source authors none.
        string ElisionInitials;
        /// @brief The elision table's words, in source order.
        vector<ParsedElisionRule> ElisionRules;
        /// @brief The messages, in source order (the encoder sorts them).
        vector<ParsedLocaleMessage> Messages;
    };

    /// @brief One parsed locale in a `*.locindex.json` index source.
    struct ParsedLocaleIndexLocale
    {
        /// @brief The locale id.
        string Id;
        /// @brief The endonym (the locale's display name in its own language).
        string DisplayName;
        /// @brief The fallback locale id (defaults to Id).
        string Fallback;
        /// @brief The locale's LocaleCatalog AssetId.
        AssetId Catalog;
    };

    /// @brief A parsed `*.locindex.json` index source.
    struct ParsedLocaleIndex
    {
        /// @brief The source locale id (the fallback terminus).
        string Source;
        /// @brief True when the index declares `"coverage": "complete"` (a missing key is a cook error).
        bool CoverageComplete = false;
        /// @brief The available locales, in authored order.
        vector<ParsedLocaleIndexLocale> Locales;
    };

    /// @brief The shared parse/validate walk for a `*.loc.json` catalog source.
    ///
    /// Backs both the catalog importer (which encodes the result) and the index importer's
    /// translation-coverage check (which reads each catalog's key set), so the two cannot diverge on
    /// what a catalog source means. A pluralized message missing its `other` variant is an error,
    /// as is a malformed `elision` object: `words` without `initials`, a word that is empty or
    /// holds a space, or a non-string value.
    /// @param doc    The parsed catalog JSON.
    /// @param label  A source label for diagnostics.
    /// @return The parsed catalog, or a located error string.
    [[nodiscard]] Result<ParsedLocaleCatalog> ParseLocaleCatalogSource(const json& doc,
                                                                       std::string_view label);

    /// @brief Encodes a parsed catalog into a CookedLocaleCatalog blob (sorted key table + pool).
    /// @param catalog  The parsed catalog.
    /// @return The cooked blob bytes.
    [[nodiscard]] vector<u8> EncodeLocaleCatalogBlob(const ParsedLocaleCatalog& catalog);

    /// @brief Parses a `*.locindex.json` index source (structure only; fallback/coverage validation
    ///        is the importer's, needing the resolver).
    /// @param doc    The parsed index JSON.
    /// @param label  A source label for diagnostics.
    /// @return The parsed index, or a located error string.
    [[nodiscard]] Result<ParsedLocaleIndex> ParseLocaleIndexSource(const json& doc,
                                                                   std::string_view label);

    /// @brief Encodes a parsed index into a CookedLocaleIndex blob (locale table + endonym pool).
    /// @param index  The parsed index.
    /// @return The cooked blob bytes, or an error when a locale id exceeds the cooked capacity.
    [[nodiscard]] Result<vector<u8>> EncodeLocaleIndexBlob(const ParsedLocaleIndex& index);
}
