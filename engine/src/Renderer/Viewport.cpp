#include <Veng/Renderer/Viewport.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    Unique<Viewport> Viewport::Create(const ViewportInfo& info)
    {
        return Unique<Viewport>(new Viewport(info));
    }

    Viewport::Viewport(const ViewportInfo& info)
        : m_Context(info.Context), m_Region(info.Region), m_Role(info.Role)
    {
        // A struct member cannot default to a value pulled from the Context&, so an
        // Undefined ColorFormat resolves to the window's output format here.
        const Format colorFormat = info.ColorFormat == Format::Undefined
                                       ? info.Context.GetOutputFormat()
                                       : info.ColorFormat;

        m_Renderer = SceneRenderer::Create({
            .Context = info.Context,
            .Assets = info.Assets,
            .OutputFormat = colorFormat,
            .Extent = info.Region.Extent,
            .Settings = info.Settings,
        });

        RefreshOutputHandle();
    }

    Viewport::~Viewport()
    {
        m_Context.GetBindlessRegistry().Release(m_OutputHandle);
    }

    void Viewport::RefreshOutputHandle()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_OutputHandle);
        m_OutputHandle = bindless.Register(m_Renderer->GetOutput());
    }

    void Viewport::SetRegion(const ViewportRegion& region)
    {
        m_Region.Offset = region.Offset;

        // A zero extent (a collapsed or first-frame panel) is ignored so it never drives
        // SceneRenderer::Resize(0,0); a real change debounces a resize to the next Render.
        if (region.Extent.x != 0 && region.Extent.y != 0 && region.Extent != m_Region.Extent)
        {
            m_Region.Extent = region.Extent;
            m_PendingExtent = region.Extent;
        }
    }

    void Viewport::SetViewState(const ViewState& state)
    {
        m_ViewState = state;
    }

    void Viewport::Configure(const SceneRendererSettings& settings)
    {
        m_Renderer->Configure(settings);
        RefreshOutputHandle();
    }

    void Viewport::Render(CommandBuffer& cmd)
    {
        if (m_PendingExtent.x != 0 && m_PendingExtent.y != 0)
        {
            m_Renderer->Resize(m_PendingExtent);
            m_PendingExtent = {};
            RefreshOutputHandle();
        }

        // A null World renders nothing: SceneView::World is a const Scene& that cannot be
        // built from a null pointer, so the early return precedes the SceneView build.
        if (m_ViewState.World == nullptr)
        {
            return;
        }

        const SceneView view{
            .World = *m_ViewState.World,
            .Camera = m_ViewState.Camera,
            .Delta = m_ViewState.Delta,
            .Exposure = m_ViewState.Exposure,
            .BloomThreshold = m_ViewState.BloomThreshold,
            .BloomIntensity = m_ViewState.BloomIntensity,
            .BloomRadius = m_ViewState.BloomRadius,
        };
        m_Renderer->Execute(cmd, view);

        // The output is sampled outside the renderer's graph (the compositor, an ImGui
        // panel, a material), so transition it to a sampleable layout here.
        cmd.PrepareForAccess(m_Renderer->GetOutput(), AccessKind::Sample);
    }

    Ref<ImageView> Viewport::GetOutput() const
    {
        return m_Renderer->GetOutput();
    }

    TextureHandle Viewport::GetOutputHandle() const
    {
        return m_OutputHandle;
    }

    const ViewportRegion& Viewport::GetRegion() const
    {
        return m_Region;
    }

    ViewportRole Viewport::GetRole() const
    {
        return m_Role;
    }

    SceneRenderer& Viewport::GetRenderer() const
    {
        return *m_Renderer;
    }
}
