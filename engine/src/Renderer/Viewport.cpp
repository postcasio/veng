#include <Veng/Renderer/Viewport.h>

#include <Veng/Assert.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Sampler.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "Passes/GuiScenePass.h"
#include "Picking.h"

#include <algorithm>

namespace Veng::Renderer
{
    Unique<Viewport> Viewport::Create(const ViewportInfo& info)
    {
        return Unique<Viewport>(new Viewport(info));
    }

    Viewport::Viewport(const ViewportInfo& info)
        : m_Context(info.Context), m_Assets(info.Assets), m_Region(info.Region),
          m_RenderScale(info.RenderScale), m_MaxAllocationScale(info.MaxAllocationScale),
          m_Role(info.Role), m_RenderOnDemand(info.RenderOnDemand), m_UiScale(info.UiScale)
    {
        VE_ASSERT(info.RenderScale > 0.0f, "Viewport RenderScale must be > 0 (got {})",
                  info.RenderScale);
        VE_ASSERT(info.UiScale > 0.0f, "Viewport UiScale must be > 0 (got {})", info.UiScale);
        VE_ASSERT(info.MaxAllocationScale > 0.0f,
                  "Viewport MaxAllocationScale must be > 0 (got {})", info.MaxAllocationScale);

        m_Id = info.Context.GetViewportRegistry().Mint(*this);

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
        // Clear each attached document's back-reference so a surviving document does not later
        // detach through a destroyed viewport. The documents are non-owning; the owner keeps them.
        for (const AttachedDocument& attached : m_Documents)
        {
            attached.Document->m_HostViewport = nullptr;
        }

        // Order-preserving erase from the drive-list (registration order is render order, so a
        // swap-and-pop would scramble it). Unregistered viewports leave m_DriveList null.
        if (m_DriveList != nullptr)
        {
            const auto removed = std::ranges::remove(*m_DriveList, this);
            m_DriveList->erase(removed.begin(), removed.end());
        }

        m_Context.GetBindlessRegistry().Release(m_OutputHandle);
        m_Context.GetBindlessRegistry().Release(m_CompositeHandle);

        m_Context.GetViewportRegistry().Retire(m_Id);
    }

    void Viewport::AttachToDriveList(vector<Viewport*>& driveList)
    {
        VE_ASSERT(m_DriveList == nullptr, "Viewport is already registered to a drive-list");
        m_DriveList = &driveList;
    }

    void Viewport::AttachDocument(Gui::Document& document, i32 layer)
    {
        VE_ASSERT(document.GetHostViewport() == nullptr,
                  "Gui::Document is already attached to a viewport; detach it first");

        // The GuiScenePass and the shared draw list are created on the first attach, so a viewport
        // that never hosts a document allocates no UI resources. The pass composites at the region's
        // native output extent, not the render-scale sub-rect, so text stays sharp.
        if (!m_GuiPass)
        {
            m_GuiPass = GuiScenePass::Create({
                .Context = m_Context,
                .Assets = m_Assets,
                .Extent = m_Region.Extent,
                .OutputFormat = m_Renderer->GetOutput()->GetImage()->GetFormat(),
            });
            m_DrawList = CreateUnique<Gui::DrawList>();
            m_GuiPassExtent = m_Region.Extent;
        }

        document.m_HostViewport = this;

        // Insert keeping the stack sorted by layer with ties in attach order: find the first entry
        // whose layer exceeds the new one and insert before it, so an equal-layer document lands
        // after the ones already at that layer.
        const auto insertAt = std::ranges::find_if(
            m_Documents, [layer](const AttachedDocument& entry) { return entry.Layer > layer; });
        m_Documents.insert(insertAt, {.Document = &document, .Layer = layer});

        RebuildDocumentPointers();
    }

    void Viewport::DetachDocument(Gui::Document& document)
    {
        const auto found =
            std::ranges::find_if(m_Documents, [&document](const AttachedDocument& entry)
                                 { return entry.Document == &document; });
        VE_ASSERT(found != m_Documents.end(), "Gui::Document is not attached to this viewport");

        document.m_HostViewport = nullptr;
        m_Documents.erase(found);
        RebuildDocumentPointers();
    }

    void Viewport::RebuildDocumentPointers()
    {
        m_DocumentPointers.clear();
        m_DocumentPointers.reserve(m_Documents.size());
        for (const AttachedDocument& attached : m_Documents)
        {
            m_DocumentPointers.push_back(attached.Document);
        }
    }

    std::span<Gui::Document* const> Viewport::GetAttachedDocuments() const
    {
        return m_DocumentPointers;
    }

    void Viewport::RefreshOutputHandle()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_OutputHandle);
        m_OutputHandle = bindless.Register(m_Renderer->GetOutput());
        ++m_OutputGeneration;
    }

    void Viewport::SetUiScale(const f32 scale)
    {
        VE_ASSERT(scale > 0.0f, "Viewport::SetUiScale: scale {} must be positive", scale);
        m_UiScale = scale;
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

    void Viewport::SetLayout(const optional<ViewportLayout>& layout)
    {
        m_Layout = layout;
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

    const optional<DynamicResolutionSettings>& Viewport::GetDynamicResolution() const
    {
        return m_DynamicResolution;
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

    const SceneRendererSettings& Viewport::GetSettings() const
    {
        return m_Renderer->GetSettings();
    }

    void Viewport::Render(CommandBuffer& cmd)
    {
        // A disabled viewport skips its whole render, keeping the prior output — the owner knows
        // it is fully occluded (a fullscreen screen presented over it) and pays nothing for it.
        if (!m_Enabled)
        {
            m_ViewStateFresh = false;
            return;
        }

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
            .Alpha = m_ViewState.Alpha,
            // The sub-rect fraction of the allocation to render this frame; the terminal tonemap
            // upscales it to the full (allocation-sized) output, so GetOutput stays full-resolution.
            .RenderScale = GetViewRenderScale(),
            .Exposure = m_ViewState.Exposure,
            .Tonemapper = m_ViewState.Tonemapper,
            .AutoExposureKey = m_ViewState.AutoExposureKey,
            .AutoExposureMinLuminance = m_ViewState.AutoExposureMinLuminance,
            .AutoExposureMaxLuminance = m_ViewState.AutoExposureMaxLuminance,
            .AutoExposureSpeed = m_ViewState.AutoExposureSpeed,
            .AutoExposureLowPercentile = m_ViewState.AutoExposureLowPercentile,
            .AutoExposureHighPercentile = m_ViewState.AutoExposureHighPercentile,
            .AmbientFloor = m_ViewState.AmbientFloor,
            .BloomThreshold = m_ViewState.BloomThreshold,
            .BloomIntensity = m_ViewState.BloomIntensity,
            .BloomRadius = m_ViewState.BloomRadius,
            .SsrIntensity = m_ViewState.SsrIntensity,
            .SsrMaxDistance = m_ViewState.SsrMaxDistance,
            .SsrThickness = m_ViewState.SsrThickness,
            .SsrMaxRoughness = m_ViewState.SsrMaxRoughness,
            .DofFocusDistance = m_ViewState.DofFocusDistance,
            .DofAperture = m_ViewState.DofAperture,
            .DofCocScale = m_ViewState.DofCocScale,
            .DofMaxCoc = m_ViewState.DofMaxCoc,
            .DofRingCount = m_ViewState.DofRingCount,
        };
        // Drive any GuiSurface panels in the scene into their HDR targets before the scene render,
        // so a translucent/emissive panel material samples a shader-readable target the same frame.
        RenderSurfaces(cmd);

        m_Renderer->Execute(cmd, view);

        // The output is sampled outside the renderer's graph (the compositor, an ImGui
        // panel, a material), so transition it to a sampleable layout here.
        cmd.PrepareForAccess(m_Renderer->GetOutput(), AccessKind::SampleGraphics);

        // Drive the GuiOverlay components this viewport claims onto its layer stack, so a
        // component-declared HUD attaches (or re-attaches) before the layers are composited below.
        DriveOverlays();

        // Drive the attached documents and blend their layers over the scene output. A viewport with
        // no documents skips this entirely — its output stays the scene output, byte-identical.
        RenderDocuments(cmd);

        ServicePendingPick();
    }

    void Viewport::RenderDocuments(CommandBuffer& cmd)
    {
        if (m_Documents.empty())
        {
            return;
        }

        // The documents lay out at the region's native output extent divided by the UI scale —
        // logical points, the sharp full-resolution surface (never the render-scale sub-rect the
        // scene renders into), magnified back up by the scale at draw. A region extent change
        // re-sizes the UI image and composite target to match before the layers solve.
        const vec2 available = vec2(m_Region.Extent) / m_UiScale;
        const f32 delta = m_ViewState.Delta;

        if (m_GuiPassExtent != m_Region.Extent)
        {
            m_GuiPass->Resize(m_Region.Extent);
            m_GuiPassExtent = m_Region.Extent;
        }
        m_GuiPass->SetUiScale(m_UiScale);
        m_GuiTime += delta;
        m_GuiPass->SetTime(m_GuiTime);

        // Walk bottom → top, driving each document's per-frame pipeline and appending its geometry
        // into the one shared draw list, so the layers composite in a single GuiScenePass record.
        m_DrawList->Clear();
        for (const AttachedDocument& attached : m_Documents)
        {
            attached.Document->Drive(available, delta, *m_DrawList);
        }

        m_GuiPass->SetDrawList(*m_DrawList);
        m_GuiPass->Render(cmd, m_Renderer->GetOutput());

        // The composite is sampled outside the pass's graph (the compositor, a material, an ImGui
        // panel), so transition it to a sampleable layout — the same handoff the scene output gets.
        cmd.PrepareForAccess(m_GuiPass->GetOutput(), AccessKind::SampleGraphics);

        // Register the composite view into bindless (once, and again whenever a resize replaced it)
        // so GetOutputHandle names the composited result.
        RefreshCompositeHandle();
    }

    void Viewport::RenderSurfaces(CommandBuffer& cmd)
    {
        if (m_ViewState.World == nullptr)
        {
            return;
        }

        // The ViewState borrows the presented scene const for rendering; a driven surface's driver is
        // the sanctioned point that reads view state and stamps request/view-output components, so
        // hand it a mutable scene here. Unlike an overlay's, this runs *ahead* of the render gather —
        // a surface is sampled by the scene it sits in, so its document must be current before the
        // gather — and a driver stamps no GuiSurface, so the walk below is undisturbed.
        auto& world = const_cast<Scene&>(*m_ViewState.World);
        for (auto [entity, surface] : world.View<GuiSurface>())
        {
            // Take the document sampler on the first surface encountered, so a scene without any
            // GuiSurface asks for nothing. Each surface owns its own pass and target.
            if (!m_SurfaceSamplerHandle.IsValid())
            {
                m_SurfaceSamplerHandle = m_Context.GetBindlessRegistry()
                                             .AcquireSampler({
                                                 .Name = "GuiSurface Sampler",
                                                 .MagFilter = Filter::Linear,
                                                 .MinFilter = Filter::Linear,
                                                 .AddressModeU = AddressMode::ClampToEdge,
                                                 .AddressModeV = AddressMode::ClampToEdge,
                                                 .AddressModeW = AddressMode::ClampToEdge,
                                             })
                                             .Handle;
            }

            // The panel binds onto its sibling MeshRenderer's first material (the mesh it draws onto).
            MaterialInstance* material = nullptr;
            if (const auto* mesh = world.TryGet<MeshRenderer>(entity); mesh != nullptr)
            {
                if (mesh->Mesh.IsLoaded())
                {
                    const std::span<const AssetHandle<MaterialInstance>> materials =
                        mesh->Mesh.Get()->GetMaterials();
                    if (!materials.empty() && materials[0].IsLoaded())
                    {
                        material = materials[0].Get();
                    }
                }
            }

            // Every presenting viewport renders the surface's document — it is one document on one
            // mesh in the world, not a per-viewport presentation — but exactly one drives its
            // driver, so a scene shown twice does not update one view-model twice.
            GuiSurfaceDriveContext driver;
            if (ClaimsSurface(world, surface))
            {
                driver = GuiSurfaceDriveContext{
                    .World = &world,
                    .Drivers = m_GuiDrivers,
                    .Owner = entity,
                    .Seat = m_Seat,
                    .Alpha = m_ViewState.Alpha,
                    .View = SystemViewInfo{.Camera = m_ViewState.Camera,
                                           .Region = m_Region,
                                           .UiScale = m_UiScale},
                };
            }

            surface.Drive(m_Context, m_Assets, cmd, m_SurfaceSamplerHandle, material,
                          m_ViewState.Delta, driver);
        }
    }

    bool Viewport::ClaimsSurface(const Scene& world, const GuiSurface& surface) const
    {
        // A surface's seat is the one it already names for input (GuiSurface::Seat); a surface that
        // names none is claimed by the sole/primary presenter, so a single-viewport cockpit drives
        // unchanged.
        if (!surface.Seat.IsNull())
        {
            return surface.Seat == m_Seat;
        }
        return IsPrimaryPresenterOf(world);
    }

    bool Viewport::IsPrimaryPresenterOf(const Scene& world) const
    {
        // An unregistered viewport driven directly is the sole presenter by definition. Otherwise the
        // primary presenter is the first viewport in registration order whose bound scene is this one
        // — so among the viewports presenting a shared scene exactly one claims an unbound overlay.
        if (m_DriveList == nullptr)
        {
            return true;
        }
        for (const Viewport* const viewport : *m_DriveList)
        {
            if (viewport->m_ViewState.World == &world)
            {
                return viewport == this;
            }
        }
        return true;
    }

    bool Viewport::ClaimsOverlay(const Scene& world, const Entity entity,
                                 const GuiOverlay& overlay) const
    {
        // The overlay's target seat is its own entity when that entity is a seat (a Viewer), else its
        // authored TargetSeat, else unbound. A bound overlay is claimed by the viewport on its seat;
        // an unbound one by the sole/primary presenter, so a single-viewport HUD attaches unchanged.
        const Entity target = world.Has<Viewer>(entity) ? entity : overlay.TargetSeat;
        if (!target.IsNull())
        {
            return target == m_Seat;
        }
        return IsPrimaryPresenterOf(world);
    }

    void Viewport::DriveOverlays()
    {
        if (m_ViewState.World == nullptr)
        {
            return;
        }

        // The ViewState borrows the presented scene const for rendering, but the engine owns it
        // mutably; a driven overlay's driver is the sanctioned point that reads finalized view state
        // and stamps request/view-output components, so hand Drive a mutable scene here. The render
        // gather already ran (Execute, above), and a driver stamps no GuiOverlay, so mutating other
        // component pools now cannot disturb this View<GuiOverlay> walk.
        auto& world = const_cast<Scene&>(*m_ViewState.World);
        for (auto [entity, overlay] : world.View<GuiOverlay>())
        {
            if (ClaimsOverlay(world, entity, overlay))
            {
                overlay.Drive(*this, m_Assets, world, entity, m_GuiDrivers);
            }
        }
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

    void Viewport::RefreshCompositeHandle()
    {
        const Ref<ImageView>& composite = m_GuiPass->GetOutput();
        if (composite == m_RegisteredCompositeView)
        {
            return;
        }

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_CompositeHandle);
        m_CompositeHandle = bindless.Register(composite);
        m_RegisteredCompositeView = composite;
        ++m_OutputGeneration;
    }

    Ref<ImageView> Viewport::GetOutput() const
    {
        // With documents attached the presented result is the UI-composited image; with none it is
        // the raw scene output, byte-identical to a viewport that never hosted a document.
        if (!m_Documents.empty())
        {
            return m_GuiPass->GetOutput();
        }
        return m_Renderer->GetOutput();
    }

    TextureHandle Viewport::GetOutputHandle() const
    {
        if (!m_Documents.empty())
        {
            return m_CompositeHandle;
        }
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

    bool Viewport::IsPointerOverDocument(const ivec2 windowPoint) const
    {
        const optional<vec2> normalized = WindowToViewport(windowPoint);
        if (!normalized)
        {
            return false;
        }

        // Document space is logical points under the viewport's UI scale — the extent a document is
        // solved at — so the region point divides by it, exactly as the Gui input consumer does.
        const f32 scale = m_UiScale > 0.0f ? m_UiScale : 1.0f;
        const vec2 documentPoint = *normalized * vec2(m_Region.Extent) / scale;

        // Top-first: the topmost layer owns the pointer, matching the routing order.
        for (auto it = m_Documents.rbegin(); it != m_Documents.rend(); ++it)
        {
            Gui::Document* const document = it->Document;
            if (document != nullptr && document->IsInteractive() &&
                document->HitTest(documentPoint) != nullptr)
            {
                return true;
            }
        }
        return false;
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

        // Unproject the near and far clip points of this pixel; the ray runs through both.
        // Reverse-Z: the near plane is NDC z = 1 and the far plane is z = 0. The origin is the
        // unprojected near point, not the camera position: under an orthographic projection
        // every pixel's ray is parallel and the camera position lies on none of them; under
        // perspective the near point sits on the eye ray, so both agree.
        const vec4 nearClip = invViewProj * vec4(ndc, 1.0f, 1.0f);
        const vec4 farClip = invViewProj * vec4(ndc, 0.0f, 1.0f);
        const vec3 nearWorld = vec3(nearClip) / nearClip.w;
        const vec3 farWorld = vec3(farClip) / farClip.w;

        return Ray{
            .Origin = nearWorld,
            .Direction = glm::normalize(farWorld - nearWorld),
        };
    }

    optional<vec2> Viewport::WorldToRegion(const vec3 world) const
    {
        // Before any ViewState the retained camera is the default view; projecting through it
        // would fabricate a position, so the contract returns nullopt instead.
        if (!m_HasViewState)
        {
            return std::nullopt;
        }
        return ProjectToScreen(m_ViewState.Camera, world, vec2(GetRegion().Extent));
    }

    optional<vec2> Viewport::WorldToDocument(const vec3 world) const
    {
        const optional<vec2> region = WorldToRegion(world);
        if (!region.has_value())
        {
            return std::nullopt;
        }
        return *region / GetUiScale();
    }

    vec2 Viewport::GetDocumentExtent() const
    {
        return vec2(GetRegion().Extent) / GetUiScale();
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
