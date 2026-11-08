// The Context members that name no device: the GPU timing readout, the frame-amortized services,
// and the setup and bindless-acquire work a frame or one-shot recording drains first. Every backend
// shares them; the rest of Context is the backend's.

#include <Veng/Renderer/Context.h>

#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GeneratedTextureService.h>

namespace Veng::Renderer
{
    bool Context::IsGpuTimingSupported() const
    {
        return m_GpuTimingSupported;
    }

    f32 Context::GetLastGpuFrameTimeMs() const
    {
        return m_GpuFrameTimeMs;
    }

    std::span<const Context::GpuPassTiming> Context::GetLastGpuPassTimings() const
    {
        return m_GpuPassTimings;
    }

    std::span<const Context::GpuPassTiming> Context::GetLastImmediateGpuPassTimings() const
    {
        return m_ImmediateGpuPassTimings;
    }

    GeneratedTextureService& Context::GetGeneratedTextures() const
    {
        return *m_GeneratedTextures;
    }

    AsyncReadback& Context::GetAsyncReadback() const
    {
        return *m_AsyncReadback;
    }

    void Context::EnqueueBindlessAcquire(const Ref<ImageView>& view)
    {
        m_PendingBindlessAcquires.push_back(view);
    }

    void Context::DrainBindlessAcquires(CommandBuffer& commandBuffer)
    {
        for (const Ref<ImageView>& view : m_PendingBindlessAcquires)
        {
            commandBuffer.PrepareForAccess(view, AccessKind::SampleAny);
        }
        m_PendingBindlessAcquires.clear();
    }

    void Context::DrainSetupCommands(CommandBuffer& commandBuffer)
    {
        if (m_PendingSetupCommands.empty())
        {
            return;
        }
        // Swapped out first: a recording may itself hand over more setup work, which records
        // directly now that a buffer is open.
        vector<std::function<void(CommandBuffer&)>> pending;
        pending.swap(m_PendingSetupCommands);
        BeginGpuScope(commandBuffer, "Setup Commands");
        for (const std::function<void(CommandBuffer&)>& commands : pending)
        {
            commands(commandBuffer);
        }
        EndGpuScope(commandBuffer);
    }
}
