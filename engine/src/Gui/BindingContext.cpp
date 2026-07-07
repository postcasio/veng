#include <Veng/Gui/BindingContext.h>

namespace Veng::Gui
{
    void BindingContext::SetData(void* object, TypeId type)
    {
        m_Data = object;
        m_DataType = object != nullptr ? type : InvalidTypeId;
        ++m_Version;
    }

    void BindingContext::SetHandler(string name, EventHandler handler)
    {
        if (handler)
        {
            m_Handlers[std::move(name)] = std::move(handler);
        }
        else
        {
            m_Handlers.erase(name);
        }
        ++m_Version;
    }

    const EventHandler* BindingContext::FindHandler(string_view name) const
    {
        const auto it = m_Handlers.find(string{name});
        return it == m_Handlers.end() ? nullptr : &it->second;
    }
}
