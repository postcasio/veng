#pragma once

#include <Veng/Renderer/Backend/CommandBuffer.h>
#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    class Command
    {
    public:
        static CommandBuffer& BeginFrame();
        static void EndFrame();

        template <typename T>
        static void SubmitResource(const Ref<T>& resource)
        {
            auto& frame = Context::Instance().GetCurrentFrame();

            frame.SubmitResource(resource);
        }

        template <typename T>
        static void SubmitResources(const vector<Ref<T>>& resources)
        {
            auto& frame = Context::Instance().GetCurrentFrame();

            for (auto& resource : resources)
            {
                frame.SubmitResource(resource);
            }
        }
    };
}
