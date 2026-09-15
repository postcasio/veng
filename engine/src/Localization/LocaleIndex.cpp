#include <Veng/Localization/LocaleIndex.h>

namespace Veng::Localization
{
    LocaleIndex::LocaleIndex(Contents contents)
        : m_SourceLocale(std::move(contents.SourceLocale)), m_Locales(std::move(contents.Locales))
    {
    }

    Ref<LocaleIndex> LocaleIndex::Create(Contents contents)
    {
        return Ref<LocaleIndex>(new LocaleIndex(std::move(contents)));
    }

    const LocaleEntry* LocaleIndex::Find(const std::string_view id) const
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
}
