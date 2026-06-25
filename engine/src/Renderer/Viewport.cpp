#include <Veng/Renderer/Viewport.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng::Renderer
{
    Unique<Viewport> Viewport::Create(const ViewportInfo& info)
    {
        return Unique<Viewport>(new Viewport(info));
    }

    Viewport::Viewport(const ViewportInfo& info)
        : m_Context(info.Context), m_Region(info.Region), m_RenderScale(info.RenderScale),
          m_Role(info.Role)
    {
        VE_ASSERT(info.RenderScale > 0.0f, "Viewport RenderScale must be > 0 (got {})",
                  info.RenderScale);

        // A struct member cannot default to a value pulled from the Context&, so an
        // Undefined ColorFormat resolves to the window's output format here.
        const Format colorFormat = info.ColorFormat == Format::Undefined
                                       ? info.Context.GetOutputFormat()
                                       : info.ColorFormat;

        m_Renderer = SceneRenderer::Create({
            .Context = info.Context,
            .Assets = info.Assets,
            .OutputFormat = colorFormat,
            .Extent = ScaledExtent(),
            .Settings = info.Settings,
        });

        RefreshOutputHandle();
    }

    Viewport::~Viewport()
    {
        // Order-preserving erase from the drive-list (registration order is render order, so a
        // swap-and-pop would scramble it). Unregistered viewports leave m_DriveList null.
        if (m_DriveList != nullptr)
        {
            const auto removed = std::ranges::remove(*m_DriveList, this);
            m_DriveList->erase(removed.begin(), removed.end());
        }

        m_Context.GetBindlessRegistry().Release(m_OutputHandle);
    }

    void Viewport::AttachToDriveList(vector<Viewport*>& driveList)
    {
        VE_ASSERT(m_DriveList == nullptr, "Viewport is already registered to a drive-list");
        m_DriveList = &driveList;
    }

    void Viewport::RefreshOutputHandle()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_OutputHandle);
        m_OutputHandle = bindless.Register(m_Renderer->GetOutput());
        ++m_OutputGeneration;
    }

    void Viewport::SetRegion(const ViewportRegion& region)
    {
        m_Region.Offset = region.Offset;

        // A zero extent (a collapsed or first-frame panel) is ignored so it never drives
        // SceneRenderer::Resize(0,0); a real change debounces a resize to the next Render.
        if (region.Extent.x != 0 && region.Extent.y != 0 && region.Extent != m_Region.Extent)
        {
            m_Region.Extent = region.Extent;
            m_PendingExtent = ScaledExtent();
        }
    }

    void Viewport::SetRenderScale(f32 scale)
    {
        VE_ASSERT(scale > 0.0f, "Viewport RenderScale must be > 0 (got {})", scale);

        if (scale == m_RenderScale)
        {
            return;
        }

        // The allocation tracks the upper-bound scale (DRS MaxScale, or the static scale when DRS
        // is off). While DRS is enabled the current scale stays at or below MaxScale, so a current-
        // scale move never changes the allocation — it only adjusts the per-frame sub-rect fraction
        // pushed through the SceneView. With DRS off the static scale *is* the upper bound, so a
        // change moves the allocation and debounces a resize.
        const uvec2 priorAlloc = ScaledExtent();
        m_RenderScale = scale;
        DebounceAllocationResize(priorAlloc);
    }

    f32 Viewport::GetRenderScale() const
    {
        return m_RenderScale;
    }

    void Viewport::SetDynamicResolution(const DynamicResolutionSettings& settings)
    {
        // The allocation is sized to MaxScale (the high-water mark the controller can reach), so a
        // MaxScale change resizes the renderer images — the point of the bound. The current scale
        // is clamped into the new band so it never exceeds the allocation between this call and the
        // next controller update (a ViewRenderScale > 1 would render outside the target).
        const uvec2 priorAlloc = ScaledExtent();
        m_DynamicResolution = settings;
        m_RenderScale = glm::clamp(m_RenderScale, settings.MinScale, settings.MaxScale);
        DebounceAllocationResize(priorAlloc);
    }

    void Viewport::ClearDynamicResolution()
    {
        // The allocation scale flips from MaxScale back to the (now static) current scale, which
        // may move the allocation extent and debounce a resize.
        const uvec2 priorAlloc = ScaledExtent();
        m_DynamicResolution.reset();
        DebounceAllocationResize(priorAlloc);
    }

    bool Viewport::IsDynamicResolutionEnabled() const
    {
        return m_DynamicResolution.has_value();
    }

    u64 Viewport::GetOutputGeneration() const
    {
        return m_OutputGeneration;
    }

    f32 Viewport::AllocationScale() const
    {
        // The allocation is sized to the upper bound of the render scale: MaxScale when the dynamic-
        // resolution controller owns the scale, else the static scale (its own ceiling). Sizing to
        // the ceiling is what lets a current-scale move render into a sub-rect without a resize, and
        // lets a MaxScale below 1 actually shrink the images rather than allocating full-region.
        return m_DynamicResolution ? m_DynamicResolution->MaxScale : m_RenderScale;
    }

    uvec2 Viewport::ExtentForScale(f32 scale) const
    {
        const vec2 allocated = glm::round(vec2(m_Region.Extent) * scale);
        return glm::max(uvec2(allocated), uvec2(1));
    }

    uvec2 Viewport::ScaledExtent() const
    {
        return ExtentForScale(AllocationScale());
    }

    f32 Viewport::ViewRenderScale() const
    {
        // The current scale as a fraction of the allocation scale (the ceiling the target is sized
        // to): at the ceiling the fraction is 1 (renders the full target), below it a sub-rect. The
        // clamp guards the window between a MaxScale drop and the next controller update.
        return glm::min(m_RenderScale / AllocationScale(), 1.0f);
    }

    void Viewport::DebounceAllocationResize(uvec2 priorAlloc)
    {
        // A zero-extent region (a collapsed or first-frame panel) never drives a resize.
        if (m_Region.Extent.x == 0 || m_Region.Extent.y == 0)
        {
            return;
        }

        const uvec2 newAlloc = ScaledExtent();
        if (newAlloc != priorAlloc)
        {
            m_PendingExtent = newAlloc;
        }
    }

    void Viewport::UpdateDynamicResolution()
    {
        if (!m_DynamicResolution || !m_Context.IsGpuTimingSupported())
        {
            return;
        }

        // The controller's scale is applied through SetRenderScale, which for a sub-rect change
        // (scale <= 1) only updates the per-frame fraction — no resize, the dynamic-resolution win.
        const f32 gpuFrameTimeMs = m_Context.GetLastGpuFrameTimeMs();
        SetRenderScale(
            ComputeDynamicResolutionScale(m_RenderScale, gpuFrameTimeMs, *m_DynamicResolution));
    }

    void Viewport::SetViewState(const ViewState& state)
    {
        m_ViewState = state;
        m_HasViewState = true;
    }

    void Viewport::Configure(const SceneRendererSettings& settings)
    {
        m_Renderer->Configure(settings);
        RefreshOutputHandle();
    }

    void Viewport::Render(CommandBuffer& cmd)
    {
        // Adaptive resolution: updates the render scale from the last frame's GPU time. A sub-rect
        // change (scale <= 1) only adjusts the per-frame fraction below — no resize; a supersample
        // boundary change leaves a pending allocation resize applied here.
        UpdateDynamicResolution();

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
            // The sub-rect fraction of the allocation to render this frame; the terminal tonemap
            // upscales it to the full (allocation-sized) output, so GetOutput stays full-resolution.
            .RenderScale = ViewRenderScale(),
            .Exposure = m_ViewState.Exposure,
            .Environment = m_ViewState.Environment,
            .EnvironmentIntensity = m_ViewState.EnvironmentIntensity,
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

    optional<vec2> Viewport::WindowToViewport(ivec2 windowPoint) const
    {
        // A zero-extent region (a collapsed or first-frame panel) has no interior to hit.
        if (m_Region.Extent.x == 0 || m_Region.Extent.y == 0)
        {
            return std::nullopt;
        }

        const ivec2 local = windowPoint - m_Region.Offset;
        const ivec2 extent = ivec2(m_Region.Extent);

        // Right/bottom edges are exclusive: a point at Offset + Extent belongs to the next
        // viewport, so the upper bound is the extent, not extent inclusive.
        if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y)
        {
            return std::nullopt;
        }

        return vec2(static_cast<f32>(local.x) / static_cast<f32>(extent.x),
                    static_cast<f32>(local.y) / static_cast<f32>(extent.y));
    }

    optional<Ray> Viewport::ScreenToWorldRay(ivec2 windowPoint) const
    {
        // Before any ViewState the retained camera is the default view; picking through it
        // would fabricate a ray, so the contract returns nullopt instead.
        if (!m_HasViewState)
        {
            return std::nullopt;
        }

        const optional<vec2> fraction = WindowToViewport(windowPoint);
        if (!fraction.has_value())
        {
            return std::nullopt;
        }

        // [0,1] (top-left origin) to NDC. Vulkan clip space has Y pointing down, and the
        // engine's projection bakes that flip in, so a top-left fraction (y=0) maps to NDC
        // y = -1 directly without a second flip here.
        const vec2 ndc = *fraction * 2.0f - 1.0f;

        const CameraView& camera = m_ViewState.Camera;
        const mat4 invViewProj = glm::inverse(camera.ViewProjection());

        // Unproject the near (z=0) and far (z=1) clip points of this pixel; the ray runs
        // through both. The origin is the camera position so a near-plane offset never
        // shifts where picking starts.
        const vec4 nearClip = invViewProj * vec4(ndc, 0.0f, 1.0f);
        const vec4 farClip = invViewProj * vec4(ndc, 1.0f, 1.0f);
        const vec3 nearWorld = vec3(nearClip) / nearClip.w;
        const vec3 farWorld = vec3(farClip) / farClip.w;

        return Ray{
            .Origin = camera.GetPosition(),
            .Direction = glm::normalize(farWorld - nearWorld),
        };
    }

    SceneRenderer& Viewport::GetRenderer() const
    {
        return *m_Renderer;
    }
}
