#include <Veng/Renderer/Command.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>

namespace Veng::Renderer
{
    CommandBuffer& Command::BeginFrame()
    {
        auto& frame = Context::Instance().AcquireNextFrame();

        Context::Instance().AcquireNextImage(frame.GetImageAvailableSemaphore());

        frame.GetInFlightFence().Reset();

        auto commandBuffer = frame.GetCommandBuffer();

        commandBuffer->Reset();

        commandBuffer->Begin();

        return *commandBuffer;
    }

    void Command::EndFrame()
    {
        auto& frame = Context::Instance().GetCurrentFrame();

        auto commandBuffer = frame.GetCommandBuffer();

        Backend::TransitionImage(*commandBuffer, *Context::Instance().GetCurrentSwapChainImage(),
                                 ImageLayout::PresentSrc);

        commandBuffer->End();

        Context::Instance().SubmitFrame(frame);

        Context::Instance().PresentFrame(frame);
    }
}
