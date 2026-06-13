#include <Veng/Renderer/Fence.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    Fence::Native& Fence::GetNative() const { return *m_Native; }

    Fence::Fence(const string& name, const bool signaled) : m_Name(name), m_Native(CreateUnique<Native>())
    {
        const vk::FenceCreateInfo fenceCreateInfo{
            .flags = signaled ? vk::FenceCreateFlagBits::eSignaled : vk::FenceCreateFlags{}
        };

        m_Native->Fence = GetVkDevice(Context::Instance()).createFence(fenceCreateInfo).value;

        DebugMarkers::MarkFence(m_Native->Fence, m_Name);
    }

    Fence::~Fence()
    {
        GetVkDevice(Context::Instance()).destroyFence(m_Native->Fence);
    }

    void Fence::Wait() const
    {
        auto result = GetVkDevice(Context::Instance()).waitForFences(m_Native->Fence, VK_TRUE,
                                                                      std::numeric_limits<u64>::max());

        VK_ASSERT(result, "Failed to wait for fence!");
    }

    void Fence::Reset() const
    {
        GetVkDevice(Context::Instance()).resetFences(m_Native->Fence);
    }
}
