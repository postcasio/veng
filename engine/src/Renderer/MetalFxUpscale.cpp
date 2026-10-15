#include "MetalFxUpscale.h"

#include <Veng/Renderer/GBuffer.h>

#include "SceneRendererIds.h"

namespace Veng::Renderer
{
    MetalFxUpscale::MetalFxUpscale(Context& context)
        : m_Context(context), m_Support(Backend::QueryMetalFxSupport(context))
    {
    }

    bool MetalFxUpscale::ResizeTemporal(const uvec2 input, const uvec2 output, const bool enabled)
    {
        if (!enabled || !m_Support.Temporal)
        {
            m_Temporal.reset();
            return !enabled;
        }
        if (m_Temporal != nullptr && m_Temporal->GetInfo().InputExtent == input &&
            m_Temporal->GetInfo().OutputExtent == output)
        {
            return true;
        }

        m_Temporal = Backend::MetalFxTemporalScaler::Create(
            m_Context, {
                           .InputExtent = input,
                           .OutputExtent = output,
                           // The whole range the device reconstructs over, so dynamic resolution
                           // reaches its floor without recreating the scaler.
                           .MaxScale = m_Support.TemporalMaxScale,
                           .ColorFormat = HdrFormat,
                           .DepthFormat = GBuffer::DepthFormat,
                           .MotionFormat = GBuffer::VelocityFormat,
                           .OutputFormat = HdrFormat,
                           .DepthReversed = true,
                       });
        m_TemporalRefused = m_Temporal == nullptr;
        return !m_TemporalRefused;
    }

    bool MetalFxUpscale::ResizeSpatial(const uvec2 input, const uvec2 output, const bool enabled)
    {
        if (!enabled || !m_Support.Spatial)
        {
            m_Spatial.reset();
            return !enabled;
        }
        if (m_Spatial != nullptr && m_Spatial->GetInfo().InputExtent == input &&
            m_Spatial->GetInfo().OutputExtent == output)
        {
            return true;
        }

        m_Spatial = Backend::MetalFxSpatialScaler::Create(m_Context, {
                                                                         .InputExtent = input,
                                                                         .OutputExtent = output,
                                                                         .ColorFormat = HdrFormat,
                                                                         .OutputFormat = HdrFormat,
                                                                     });
        m_SpatialRefused = m_Spatial == nullptr;
        return !m_SpatialRefused;
    }

    f32 MetalFxUpscale::GetTemporalRenderScaleFloor() const
    {
        if (m_Temporal == nullptr)
        {
            return 0.0f;
        }
        const Backend::MetalFxTemporalInfo& info = m_Temporal->GetInfo();
        const vec2 floor = vec2(info.OutputExtent) / (vec2(info.InputExtent) * info.MaxScale);
        return glm::max(floor.x, floor.y);
    }
}
