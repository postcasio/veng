#include <Veng/Renderer/Semaphore.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>


namespace Veng::Renderer
{
    /// @brief Returns the backend-native semaphore handle.
    Semaphore::Native& Semaphore::GetNative() const { return *m_Native; }

    /// @brief Creates a binary Vulkan semaphore.
    /// @param context  The owning render context.
    /// @param name     Debug label attached via debug markers.
    Semaphore::Semaphore(Context& context, const string& name) : m_Context(context), m_Name(name), m_Native(CreateUnique<Native>())
    {
        constexpr vk::SemaphoreCreateInfo semaphoreCreateInfo{};

        m_Native->Semaphore = GetVkDevice(m_Context).createSemaphore(semaphoreCreateInfo).value;

        DebugMarkers::MarkSemaphore(GetVkDevice(m_Context), m_Native->Semaphore, m_Name);
    }

    /// @brief Destroys the semaphore immediately.
    ///
    /// Binary semaphores are frame-synchronized by the caller (image-available and render-finished
    /// semaphores are per-synchronization-frame), so no deferred destruction is needed.
    Semaphore::~Semaphore()
    {
        GetVkDevice(m_Context).destroySemaphore(m_Native->Semaphore);
    }
}
