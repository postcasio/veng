#include <Veng/Scene/PresentationScope.h>

#include <Veng/Assert.h>

#include <algorithm>

namespace Veng
{
    // ---- PresentationScope -------------------------------------------------------------------------

    PresentationScope::PresentationScope(PresentationScopes& registry, const PresentationScopeId id)
        : m_Registry(&registry), m_Id(id)
    {
    }

    PresentationScope::~PresentationScope()
    {
        m_Registry->Close(m_Id);
    }

    void PresentationScope::Renew(const bool audible)
    {
        m_Registry->Renew(m_Id, audible);
    }

    // ---- PresentationScopes ------------------------------------------------------------------------

    PresentationScopes::PresentationScopes()
    {
        m_Application = PresentationScopeId{.Value = m_NextId++};
        m_Records.push_back(Record{.Id = m_Application, .State = PresentationState::Live});
    }

    PresentationScopes::~PresentationScopes()
    {
        // Each handle closes into this registry, so one still open would close into freed memory.
        VE_ASSERT(
            m_Records.size() == 1,
            "PresentationScopes destroyed with {} scope(s) still open; its owner must outlive "
            "every scene holding one",
            m_Records.size() - 1);
    }

    Unique<PresentationScope> PresentationScopes::Open()
    {
        const PresentationScopeId id{.Value = m_NextId++};
        m_Records.push_back(Record{.Id = id});
        // Private constructor: raw new, not CreateUnique.
        return Unique<PresentationScope>(new PresentationScope(*this, id));
    }

    const PresentationScopes::Record* PresentationScopes::Find(const PresentationScopeId id) const
    {
        const auto it = std::ranges::lower_bound(m_Records, id.Value, {},
                                                 [](const Record& r) { return r.Id.Value; });
        return it != m_Records.end() && it->Id == id ? &*it : nullptr;
    }

    PresentationState PresentationScopes::GetState(const PresentationScopeId id) const
    {
        const Record* record = Find(id);
        return record != nullptr ? record->State : PresentationState::Closed;
    }

    void PresentationScopes::Renew(const PresentationScopeId id, const bool audible)
    {
        const auto it = std::ranges::lower_bound(m_Records, id.Value, {},
                                                 [](const Record& r) { return r.Id.Value; });
        VE_ASSERT(it != m_Records.end() && it->Id == id,
                  "PresentationScopes: renewing scope {}, which is not open", id.Value);
        it->Renewed = true;
        it->Audible = it->Audible || audible;
    }

    void PresentationScopes::Close(const PresentationScopeId id)
    {
        std::erase_if(m_Records, [id](const Record& r) { return r.Id == id; });
    }

    void PresentationScopes::Resolve()
    {
        for (Record& record : m_Records)
        {
            if (record.Id == m_Application)
            {
                continue;
            }
            if (!record.Renewed)
            {
                record.State = PresentationState::Held;
            }
            else
            {
                record.State = record.Audible ? PresentationState::Live : PresentationState::Muted;
            }
            record.Renewed = false;
            record.Audible = false;
        }
    }

    vector<PresentationScopeStatus> PresentationScopes::GetScopes() const
    {
        vector<PresentationScopeStatus> out;
        out.reserve(m_Records.size());
        for (const Record& record : m_Records)
        {
            out.push_back(PresentationScopeStatus{.Id = record.Id, .State = record.State});
        }
        return out;
    }
}
