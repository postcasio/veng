#pragma once

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

namespace Veng::Renderer
{
    class Command
    {
    public:
        static CommandBuffer& BeginFrame();
        static void EndFrame();
    };
}
