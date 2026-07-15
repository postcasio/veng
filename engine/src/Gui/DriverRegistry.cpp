#include <Veng/Gui/DriverRegistry.h>

namespace Veng
{
    Unique<GuiDriver> GuiDriverRegistry::Instantiate(const GuiDriverId id) const
    {
        for (const GuiDriverEntry& entry : m_Entries)
        {
            if (entry.Id == id)
            {
                return entry.Factory();
            }
        }
        return nullptr;
    }

    usize GuiDriverRegistry::Count() const
    {
        return m_Entries.size();
    }
}
