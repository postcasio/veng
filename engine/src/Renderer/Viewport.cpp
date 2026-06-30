#include <Veng/Renderer/Viewport.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include <Veng/Scene/Scene.h>

#include "Picking.h"

#include <algorithm>

namespace Veng::Renderer
{
    Unique<Viewport> Viewport::Create(const ViewportInfo& info)
    {
        return Unique<Viewport>(new Viewport(info));
    }

    Viewport::Viewport(const ViewportInfo& info)
        : m_Context(info.Context), m_Region(info.Region), m_RenderScale(info.RenderScale),
          m_MaxAllocationScale(info.MaxAllocationScale), m_Role(info.Role),
          m_RenderOnDemand(info.RenderOnDemand)
    {
        VE_ASSERT(info.RenderScale > 0.0f, "Viewport RenderScale must be > 0 (got {})",
                  info.RenderScale);
        VE_ASSERT(info.MaxAllocationScale > 0.0f,
                  "Viewport MaxAllocationScale must be > 0 (got {})", info.MaxAllocationScale);

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
        // The allocation is sized to the controller's MaxScale ceiling, so engaging it may move the
        // allocation extent and resize the renderer images. The current scale is clamped into the new
        // band so it never exceeds the allocation (a GetViewRenderScale > 1 would render outside).
        const uvec2 priorAlloc = ScaledExtent();
        m_DynamicResolution = settings;
        m_RenderScale = glm::clamp(m_RenderScale, settings.MinScale, settings.MaxScale);
        DebounceAllocationResize(priorAlloc);
    }

    void Viewport::ClearDynamicResolution()
    {
        // The allocation scale flips from the controller's ceiling back to the (now static) current
        // scale, which may move the allocation extent and debounce a resize.
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

    f32 Viewport::GetAllocationScale() const
    {
        // The allocation is sized to the upper bound of the render scale: MaxScale when the
        // controller owns the scale, else the static scale (its own ceiling). Sizing to the ceiling
        // lets a current-scale move render into a sub-rect without a resize, and lets a sub-1 ceiling
        // actually shrink the images rather than allocating full-region.
        return m_DynamicResolution ? m_DynamicResolution->MaxScale : m_RenderScale;
    }

    uvec2 Viewport::GetAllocationExtent() const
    {
        return ScaledExtent();
    }

    uvec2 Viewport::ExtentForScale(f32 scale) const
    {
        // MaxAllocationScale is the outermost factor: it caps the region before the upper-bound
        // allocation scale. At the default 1.0 the allocation is the full region (native resolution
        // on a HiDPI backing extent); a lower ceiling bounds it below that.
        const vec2 allocated = glm::round(vec2(m_Region.Extent) * m_MaxAllocationScale * scale);
        return glm::max(uvec2(allocated), uvec2(1));
    }

    uvec2 Viewport::ScaledExtent() const
    {
        return ExtentForScale(GetAllocationScale());
    }

    f32 Viewport::GetViewRenderScale() const
    {
        // The current scale as a fraction of the allocation scale (the ceiling the target is sized
        // to): at the ceiling the fraction is 1 (renders the full target), below it a sub-rect. The
        // clamp guards the window between a MaxScale drop and the next controller update.
        return glm::min(m_RenderScale / GetAllocationScale(), 1.0f);
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
        // A change to the bound scene bumps the pick epoch, so an in-flight pick resolved against
        // the old scene bails rather than landing an id in a swapped/cleared one.
        if (state.World != m_ViewState.World)
        {
            ++m_SceneEpoch;
        }
        m_ViewState = state;
        m_HasViewState = true;
        m_ViewStateFresh = true;
    }

    void Viewport::Configure(const SceneRendererSettings& settings)
    {
        m_Renderer->Configure(settings);
        RefreshOutputHandle();
    }

    void Viewport::Render(CommandBuffer& cmd)
    {
        // An on-demand viewport renders only on a frame its owner pushed a fresh ViewState. A hidden
        // editor panel does not draw, so it pushes none and this skips its render — the viewport
        // keeps its prior output but stops re-rendering (and writing the shared bindless targets)
        // behind the visible panels. The flag is consumed each frame regardless of the mode.
        const bool wasFresh = m_ViewStateFresh;
        m_ViewStateFresh = false;
        if (m_RenderOnDemand && !wasFresh)
        {
            return;
        }

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
            .RenderScale = GetViewRenderScale(),
            .Exposure = m_ViewState.Exposure,
            .Environment = m_ViewState.Environment,
            .EnvironmentIntensity = m_ViewState.EnvironmentIntensity,
            .AtmosphereEnabled = m_ViewState.AtmosphereEnabled,
            .SunDirection = m_ViewState.SunDirection,
            .Atmosphere = m_ViewState.Atmosphere,
            .BloomThreshold = m_ViewState.BloomThreshold,
            .BloomIntensity = m_ViewState.BloomIntensity,
            .BloomRadius = m_ViewState.BloomRadius,
        };
        m_Renderer->Execute(cmd, view);

        // The output is sampled outside the renderer's graph (the compositor, an ImGui
        // panel, a material), so transition it to a sampleable layout here.
        cmd.PrepareForAccess(m_Renderer->GetOutput(), AccessKind::Sample);

        ServicePendingPick();
    }

    void Viewport::ServicePendingPick()
    {
        if (!m_PendingPick)
        {
            return;
        }

        // The scene was swapped or cleared since the pick was issued: bail with nullopt rather than
        // resolving an id against a different scene.
        const bool sceneMatches =
            m_PendingPick->World == m_ViewState.World && m_PendingPick->Epoch == m_SceneEpoch;
        if (!sceneMatches)
        {
            const function<void(optional<Entity>)> callback = std::move(m_PendingPick->OnResolved);
            m_PendingPick.reset();
            if (callback)
            {
                callback(std::nullopt);
            }
            return;
        }

        // Forward the texel to the renderer's picking pass on the first serviced frame (this Execute
        // just ran the picking pass and staged the readback only if a request was already pending,
        // so the request is recorded here and serviced by the next Execute's staged copy).
        if (!m_PendingPick->Forwarded)
        {
            m_Renderer->RequestPick(m_PendingPick->Texel);
            m_PendingPick->Forwarded = true;
            return;
        }

        // Poll the renderer for the resolved pick id; nullopt means the readback is still in flight.
        const optional<u32> pickId = m_Renderer->PollPickId();
        if (!pickId)
        {
            return;
        }

        // Map the pick id (packed slot index + 1) back to the live entity, validating liveness at
        // resolve time. NoEntityId (0) is background; any other value's index is pickId - 1.
        optional<Entity> resolved;
        if (*pickId != Picking::NoEntityId && m_ViewState.World != nullptr)
        {
            const Entity entity = m_ViewState.World->GetLiveEntityAtIndex(*pickId - 1u);
            if (!entity.IsNull())
            {
                resolved = entity;
            }
        }

        const function<void(optional<Entity>)> callback = std::move(m_PendingPick->OnResolved);
        m_PendingPick.reset();
        if (callback)
        {
            callback(resolved);
        }
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

    void Viewport::Pick(ivec2 windowPoint, function<void(optional<Entity>)> onResolved)
    {
        // Off the region or with a zero-extent panel: an immediate background result.
        const optional<vec2> normalized = WindowToViewport(windowPoint);
        if (!normalized || m_ViewState.World == nullptr)
        {
            if (onResolved)
            {
                onResolved(std::nullopt);
            }
            return;
        }

        // The id target is allocation-sized but the picking pass renders into the valid sub-rect, so
        // map the normalized point into the rendered region (GetValidExtent), clamped to its edge.
        const uvec2 valid = m_Renderer->GetValidExtent();
        const uvec2 texel{
            std::min(static_cast<u32>(normalized->x * static_cast<f32>(valid.x)),
                     valid.x > 0 ? valid.x - 1 : 0),
            std::min(static_cast<u32>(normalized->y * static_cast<f32>(valid.y)),
                     valid.y > 0 ? valid.y - 1 : 0),
        };

        // The latest click wins: replace any still-pending pick (firing its callback would imply a
        // resolve it never got; dropping it is the "one pick in flight" posture).
        m_PendingPick = PendingPick{
            .Texel = texel,
            .Forwarded = false,
            .World = m_ViewState.World,
            .Epoch = m_SceneEpoch,
            .OnResolved = std::move(onResolved),
        };
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

    DebugDraw& Viewport::GetDebugDraw() const
    {
        return m_Renderer->GetDebugDraw();
    }
}
