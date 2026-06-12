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
    };
}
