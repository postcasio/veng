#include <Veng/Localization/LocaleCatalog.h>

#include <algorithm>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng::Localization
{
    LocaleCatalog::LocaleCatalog(Contents contents)
        : m_LocaleId(std::move(contents.LocaleId)), m_FallbackId(std::move(contents.FallbackId)),
          m_PluralRuleId(std::move(contents.PluralRuleId)), m_Numbers(contents.Numbers),
          m_Entries(std::move(contents.Entries))
    {
    }

    Ref<LocaleCatalog> LocaleCatalog::Create(Contents contents)
    {
        return Ref<LocaleCatalog>(new LocaleCatalog(std::move(contents)));
    }

    const Message* LocaleCatalog::FindMessage(const std::string_view key) const
    {
        const u64 hash = HashLocaleKey(key);

        // Binary search for the first entry with this hash, then scan the equal-hash run comparing
        // keys — so a hash collision resolves to the entry whose key actually matches, never a
        // neighbour that merely hashes alike.
        const auto begin = m_Entries.begin();
        const auto end = m_Entries.end();
        auto it = std::lower_bound(begin, end, hash, [](const LocaleCatalogEntry& entry, u64 value)
                                   { return entry.KeyHash < value; });
        for (; it != end && it->KeyHash == hash; ++it)
        {
            if (it->Key == key)
            {
                return &it->Message;
            }
        }
        return nullptr;
    }
}
