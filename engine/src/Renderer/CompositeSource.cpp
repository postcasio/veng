#include "CompositeSource.h"

namespace Veng::Renderer
{
    CompositeSceneSource
    ResolveCompositeSceneSource(const std::span<const CompositePlacement> placements,
                                const uvec2 swapChainExtent)
    {
        if (placements.empty())
        {
            return CompositeSceneSource::Black;
        }

        const ViewportRegion& last = placements.back().Region;
        const bool coversWindow = last.Offset == ivec2(0) && last.Extent == swapChainExtent;
        return coversWindow ? CompositeSceneSource::Direct : CompositeSceneSource::Gather;
    }

    CompositeOverlaySource ResolveCompositeOverlaySource(const bool layerDrew)
    {
        return layerDrew ? CompositeOverlaySource::Layer : CompositeOverlaySource::Transparent;
    }
}
