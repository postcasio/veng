#include <Veng/Localization/Localization.h>

#include <unordered_set>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Log.h>

namespace Veng::Localization
{
    namespace
    {
        // The plain string a Get returns for a message: its template, or the Other plural variant.
        std::string_view DisplayString(const Message& message)
        {
            if (!message.Plurals.has_value())
            {
                return message.Template;
            }
            return (*message.Plurals)[static_cast<usize>(PluralCategory::Other)];
        }
    }

    Localization::Localization(AssetManager& assets, const LocaleIndex& index,
                               const std::string_view chosenLocale)
        : m_Assets(&assets), m_Locales(index.GetLocales()),
          m_SourceLocale(string(index.GetSourceLocale()))
    {
        // A chosen locale the index does not name resolves to the source locale rather than leaving
        // an empty active chain.
        const std::string_view active =
            FindLocale(chosenLocale) != nullptr ? chosenLocale : std::string_view(m_SourceLocale);
        m_ActiveLocale = string(active);
        BuildChain(m_ActiveLocale);
    }

    const LocaleEntry* Localization::FindLocale(const std::string_view id) const
    {
        for (const LocaleEntry& entry : m_Locales)
        {
            if (entry.Id == id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    void Localization::BuildChain(const std::string_view activeLocale)
    {
        m_Chain.clear();
        m_Numbers = NumberFormat{};
        m_PluralRule = PluralRuleFor(activeLocale);

        if (m_Assets == nullptr)
        {
            return;
        }

        // Walk the fallback edges from the active locale, holding each locale's catalog resident.
        // The visited-set bounds a cycle and an edge to an absent locale — both are cook errors the
        // LocaleIndexImporter rejects, so the guard is defence rather than an expected path.
        std::unordered_set<string> visited;
        string current(activeLocale);
        while (visited.insert(current).second)
        {
            const LocaleEntry* entry = FindLocale(current);
            if (entry == nullptr)
            {
                Log::Error("localization: locale '{}' is not in the index; the fallback chain ends "
                           "here",
                           current);
                break;
            }

            const AssetResult<AssetHandle<LocaleCatalog>> loaded =
                m_Assets->LoadSync<LocaleCatalog>(entry->Catalog);
            if (!loaded)
            {
                Log::Error("localization: catalog for locale '{}' did not load ({})", current,
                           loaded.error().Detail);
                break;
            }
            m_Chain.push_back(*loaded);

            if (entry->Fallback == current)
            {
                break; // A terminal locale (its fallback is itself) ends the walk.
            }
            current = entry->Fallback;
        }

        // The active locale's presentation drives plurals and number separators.
        if (!m_Chain.empty() && m_Chain.front().Get() != nullptr)
        {
            const LocaleCatalog& active = *m_Chain.front().Get();
            m_Numbers = active.GetNumbers();
            const std::string_view rule = active.GetPluralRuleId();
            m_PluralRule = PluralRuleFor(rule.empty() ? activeLocale : rule);
        }
    }

    const Message* Localization::Resolve(const std::string_view key) const
    {
        for (const AssetHandle<LocaleCatalog>& handle : m_Chain)
        {
            if (const LocaleCatalog* catalog = handle.Get())
            {
                if (const Message* message = catalog->FindMessage(key))
                {
                    return message;
                }
            }
        }
        return nullptr;
    }

    std::string_view Localization::Get(const std::string_view key) const
    {
        if (const Message* message = Resolve(key))
        {
            const std::string_view display = DisplayString(*message);
            return display.empty() ? key : display;
        }
        return key;
    }

    string Localization::Format(const std::string_view key, const std::span<const FormatArg> args,
                                const optional<i64> count) const
    {
        const Message* message = Resolve(key);

        // A missing key formats the key string itself: a template with no fields formats to the key
        // verbatim, so this is the same visible-key posture Get takes.
        Message fallback;
        if (message == nullptr)
        {
            fallback.Template = string(key);
            message = &fallback;
        }

        const Result<string> formatted =
            FormatMessage(*message, args, count, m_PluralRule, m_Numbers);
        if (formatted)
        {
            return *formatted;
        }

        // A malformed template or a bad spec degrades to the raw template with its {name} markers
        // left literal, never a crash or a blank. Logged once so a bad translation is diagnosable
        // without flooding the log every frame it renders.
        if (!m_FormatErrorLogged)
        {
            Log::Warn("localization: formatting key '{}' failed ({}); showing the raw template",
                      key, formatted.error());
            m_FormatErrorLogged = true;
        }
        return string(DisplayString(*message));
    }

    void Localization::SetLocale(const std::string_view id)
    {
        if (m_Assets == nullptr)
        {
            return;
        }
        const std::string_view active =
            FindLocale(id) != nullptr ? id : std::string_view(m_SourceLocale);
        m_ActiveLocale = string(active);
        BuildChain(m_ActiveLocale);
        ++m_Generation;
    }
}
