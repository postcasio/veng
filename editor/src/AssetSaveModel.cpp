#include <VengEditor/AssetSaveModel.h>

namespace VengEditor
{
    using namespace Veng;

    void CookGate::Request(function<void()> submit)
    {
        if (m_Cooking)
        {
            m_Queued = std::move(submit);
            return;
        }

        m_Cooking = true;
        submit();
    }

    void CookGate::Complete()
    {
        m_Cooking = false;
        if (!m_Queued)
        {
            return;
        }

        // Clear the slot before re-entering Request: the released submit may itself queue behind
        // the cook it starts, and it must not find its predecessor still parked there.
        function<void()> queued = std::move(m_Queued);
        m_Queued = {};
        Request(std::move(queued));
    }

    VoidResult SaveAssetSource(const function<VoidResult()>& write, bool& dirty,
                               const function<void()>& recook)
    {
        const VoidResult written = write();
        if (!written)
        {
            return written;
        }

        dirty = false;
        recook();
        return {};
    }
}
