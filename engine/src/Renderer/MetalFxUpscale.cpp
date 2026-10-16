#include "MetalFxUpscale.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "SceneRendererIds.h"

namespace Veng::Renderer
{
    namespace
    {
        // The motion pass's fragment shader; its vertex stage is the shared fullscreen one.
        constexpr AssetId MetalFxMotionFragId{0x580CD768F662625DULL};
    }

    MetalFxUpscale::MetalFxUpscale(Context& context, AssetManager& assets)
        : m_Context(context), m_Support(Backend::QueryMetalFxSupport(context))
    {
        // Only a device that runs the temporal scaler ever wires the motion pass.
        if (!m_Support.Temporal)
        {
            return;
        }
        const auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "MetalFxUpscale: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };
        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> fs = LoadShader(MetalFxMotionFragId, "MetalFX motion");

        m_MotionLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer MetalFX Motion Layout",
                           .PushConstantRanges = {PushConstantRange::Of<MetalFxMotionPush>(
                               ShaderStage::Fragment)},
                       });
        m_MotionPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer MetalFX Motion Pipeline",
                           .ColorAttachments = {{.Format = GBuffer::VelocityFormat}},
                           .PipelineLayout = m_MotionLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                               },
                       });
    }

    MetalFxUpscale::~MetalFxUpscale() = default;

    void MetalFxUpscale::ResizeMotion(const uvec2 extent, const bool enabled)
    {
        if (!enabled)
        {
            m_MotionImage.reset();
            m_MotionView.reset();
            return;
        }
        if (m_MotionImage != nullptr && uvec2(m_MotionImage->GetExtent()) == extent)
        {
            return;
        }
        m_MotionImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer MetalFX Motion",
                                                     .Extent = {extent.x, extent.y, 1},
                                                     .Format = GBuffer::VelocityFormat,
                                                     .Usage = GBuffer::ColorUsage,
                                                     .PlatformExport = true,
                                                 });
        m_MotionView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer MetalFX Motion View", .Image = m_MotionImage});
    }

    bool MetalFxUpscale::ResizeTemporal(const uvec2 input, const uvec2 output, const bool enabled)
    {
        if (!enabled || !m_Support.Temporal)
        {
            m_Temporal.reset();
            ResizeMotion(input, false);
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
        ResizeMotion(input, !m_TemporalRefused);
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
