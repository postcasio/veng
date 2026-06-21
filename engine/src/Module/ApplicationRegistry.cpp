#include <Veng/Module/ApplicationRegistry.h>

#include <Veng/Application.h>
#include <Veng/Assert.h>

namespace Veng
{
    void ApplicationRegistry::RegisterApplication(
        function<Unique<Application>(TypeRegistry&, SystemRegistry&)> factory)
    {
        VE_ASSERT(!m_Factory, "an Application factory is already registered — one app per module");
        m_Factory = std::move(factory);
    }

    bool ApplicationRegistry::HasApplication() const
    {
        return static_cast<bool>(m_Factory);
    }

    Unique<Application> ApplicationRegistry::Create(TypeRegistry& types,
                                                    SystemRegistry& systems) const
    {
        if (!m_Factory)
        {
            return nullptr;
        }

        return m_Factory(types, systems);
    }
}
