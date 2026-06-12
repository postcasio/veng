#include <Veng/Renderer/Backend/Command.h>

#include <Veng/Renderer/Backend/Context.h>
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

        commandBuffer->PipelineBarrier({
            .Image = *Context::Instance().GetSwapChain().GetCurrentImage(),
            .NewLayout = vk::ImageLayout::ePresentSrcKHR
        });

        commandBuffer->End();

        Context::Instance().SubmitFrame(frame);

        Context::Instance().PresentFrame(frame);
    }
}
