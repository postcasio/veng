#include "SsrScenePass.h"

#include "../SsrChain.h"

namespace Veng::Renderer
{
    void SsrScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        m_Chain.Declare(graph, SsrGraphInputs{
                                   .Depth = io.GBufferDepth,
                                   .Normal = io.GBufferNormal,
                                   .Material = io.GBufferOrm,
                                   .DepthHandle = io.DepthHandle,
                                   .NormalHandle = io.NormalHandle,
                                   .MaterialHandle = io.OrmHandle,
                                   .Color = io.Hdr,
                                   .ColorHandle = io.HdrHandle,
                                   .Velocity = io.Velocity,
                                   .VelocityHandle = io.VelocityHandle,
                               });
    }
}
