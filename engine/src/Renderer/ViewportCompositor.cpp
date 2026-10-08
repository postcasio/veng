#include <Veng/Renderer/ViewportCompositor.h>

#include <Veng/Assert.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneCapturePool.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Window.h>

#include "CaptureDrive.h"
#include "CaptureRotation.h"
#include "CompositeSource.h"

#include <algorithm>

#include <glm/glm.hpp>

namespace Veng::Renderer
{
    namespace
    {
        /// @brief Creates a 1×1 colour-attachment image and its view, to be cleared to a constant.
        Ref<ImageView> CreateStandIn(Context& context, const string& name)
        {
            const Ref<Image> image = Image::Create(
                context, {
                             .Name = name,
                             .Extent = {1, 1, 1},
                             .Format = Format::RGBA16Sfloat,
                             .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                         });
            return ImageView::Create(context, {.Name = name + " View", .Image = image});
        }
    }

    ViewportCompositor::ViewportCompositor(Context& context)
        : m_Context(context), m_CapturePool(CreateRef<SceneCapturePool>())
    {
    }

    ViewportCompositor::~ViewportCompositor()
    {
        SetCaptureSink(nullptr);

        // Release the tail's GPU resources while the context is still live. The placement cache retains
        // a Ref to each Presented viewport's output view for change-detection; clearing it here — after
        // the managed viewports have already dropped, since they are declared after the compositor —
        // releases those outputs so the images retire rather than outliving the context's allocator.
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_GatherGraph.reset();
        m_Gather.reset();
        m_GatheredPlacements.clear();
        m_SceneSource.reset();
        m_BlackView.reset();
        m_TransparentView.reset();
    }

    void ViewportCompositor::InitializeTail(AssetManager& assets, ImGuiLayer& imgui)
    {
        m_Assets = &assets;
        m_ImGui = &imgui;

        m_Gather = GatherPass::Create({
            .Context = m_Context,
            .Assets = assets,
            .Extent = m_Context.GetSwapChainExtent(),
        });

        // Clamp-to-edge sampling of a 1×1 image is its one texel everywhere, so these stand in for
        // a full-window black scene and a full-window transparent overlay with no shader change.
        m_BlackView = CreateStandIn(m_Context, "Composite Black Scene");
        m_TransparentView = CreateStandIn(m_Context, "Composite Transparent Overlay");
        m_Context.ImmediateCommands(
            [this](CommandBuffer& cmd)
            {
                RenderGraph graph(m_Context);
                const ResourceId black = graph.Import("Composite Black Scene");
                const ResourceId transparent = graph.Import("Composite Transparent Overlay");
                graph.AddPass("Clear Composite Stand-ins")
                    .Color({
                        .Resource = black,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Color({
                        .Resource = transparent,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding bindings[] = {
                    {.Id = black, .View = m_BlackView},
                    {.Id = transparent, .View = m_TransparentView},
                };
                graph.Compile()->Execute(cmd, bindings);
                cmd.PrepareForAccess(m_BlackView, AccessKind::SampleGraphics);
                cmd.PrepareForAccess(m_TransparentView, AccessKind::SampleGraphics);
            });

        m_SceneSource = m_Gather->GetOutput();
        m_Composite = SwapChainCompositePass::Create({
            .Context = m_Context,
            .ImGui = &imgui,
            .Assets = assets,
            .SceneSource = m_SceneSource,
            .SwapChainFormat = m_Context.GetSwapChainFormat(),
            .ColorSpace = m_Context.GetActiveDisplayColorSpace(),
        });

        const auto compileGather = [this]
        {
            RenderGraph graph(m_Context);
            return m_Gather->Compile(graph);
        };
        const auto compileComposite = [this]
        {
            RenderGraph graph(m_Context);
            const ResourceId swapId = graph.Import("SwapChain");
            return m_Composite->Compile(graph, swapId);
        };

        // Swapchain recreation invalidates the baked extent and may re-negotiate the surface's
        // format/color space (a window moved to a display with different HDR support); re-target
        // the composite before recompiling.
        m_Context.AddSwapChainInvalidationCallback(
            [this, compileGather, compileComposite]
            {
                // Only the gather's own view is replaced here; a directly sampled source is
                // re-pointed by the next Composite if the resize moves it.
                const bool sourcedFromGather = m_SceneSource == m_Gather->GetOutput();
                m_Gather->Resize(m_Context.GetSwapChainExtent());
                if (sourcedFromGather)
                {
                    ApplySceneSource(m_Gather->GetOutput());
                }
                // The ImGui layer's invalidation callback (registered earlier, so it ran first)
                // recreated its offscreen image; re-point the composite at it or it samples the
                // retired one (old size → squished, stale content → frozen overlay).
                m_Composite->RefreshImGuiSource();
                m_Composite->SetSwapChainTarget(m_Context.GetSwapChainFormat(),
                                                m_Context.GetActiveDisplayColorSpace());
                m_GatherGraph = compileGather();
                m_CompositeGraph = compileComposite();

                // The capture pass renders at the presented extent, so a resize invalidates it
                // exactly as it does the presented composite. It is rebuilt lazily from the next
                // target the sink hands over.
                ReleaseCapturePass();
            });

        m_GatherGraph = compileGather();
        m_CompositeGraph = compileComposite();
    }

    void ViewportCompositor::RegisterViewport(Viewport& viewport)
    {
        VE_ASSERT(std::ranges::find(m_Viewports, &viewport) == m_Viewports.end(),
                  "Viewport is already registered to the compositor's drive-list");

        m_Viewports.emplace_back(&viewport);
        viewport.AttachToDriveList(m_Viewports);
        viewport.SetDevices(m_Devices);
    }

    void ViewportCompositor::SetDevices(const ViewportDevices& devices)
    {
        m_Devices = devices;
        for (Viewport* viewport : m_Viewports)
        {
            viewport->SetDevices(devices);
        }
    }

    void ViewportCompositor::RegisterCapture(SceneCapture& capture)
    {
        VE_ASSERT(std::ranges::find(m_Captures, &capture) == m_Captures.end(),
                  "SceneCapture is already registered to the compositor's drive-list");

        m_Captures.emplace_back(&capture);
        capture.AttachToDriveList(m_Captures);
    }

    void ViewportCompositor::RenderRegistered(CommandBuffer& cmd)
    {
        // The viewports about to render decide which scenes' captures are worth a face this frame, so
        // the pre-pass reads them before any Render consumes its push.
        DriveCaptureSurfaces();

        // Scene captures render first, so a material sampling a capture's output reads this frame's
        // result during the viewport renders that follow. Rendering first is also what puts them
        // ahead of the viewports in the frame's view budget, so the drive spends only what it can
        // leave the viewports: a missing reflection is a blemish, a viewport that could not claim a
        // slot is a stale window. One slot per registered viewport is the floor, which a viewport
        // whose sky re-bakes this frame still exceeds — that bake gives way instead and retries.
        DriveCaptures(cmd);

        // Then every registered viewport in registration order (each does its own Execute + Sample
        // barrier), so viewport outputs are in Sample layout before a later consumer samples them.
        for (Viewport* viewport : m_Viewports)
        {
            viewport->Render(cmd);
        }
    }

    void ViewportCompositor::DriveCaptureSurfaces()
    {
        VE_PROFILE_SCOPE("Capture/DriveSurfaces");
        m_CaptureSurfaceDrive = {};

        vector<CapturePresenter> presenters;
        presenters.reserve(m_Viewports.size());
        for (const Viewport* viewport : m_Viewports)
        {
            presenters.push_back({
                .WillRender = viewport->WillRender(),
                .Presented = viewport->GetPresentedScene(),
                .Pending = viewport->GetPendingScene(),
            });
        }
        vector<CaptureClaim> claims;
        ClaimCaptureScenes(presenters, claims);

        u32 built = 0;
        for (const CaptureClaim& claim : claims)
        {
            Viewport& viewport = *m_Viewports[claim.Presenter];
            const f32 alpha = claim.Pending ? viewport.GetPendingAlpha() : viewport.GetViewAlpha();
            // A viewport borrows its scene const for rendering; the drive is the sanctioned point that
            // installs a capturing entity's material clone, as RenderSurfaces drives its drivers.
            DriveSceneCaptures(const_cast<Scene&>(*claim.World), alpha, viewport.m_Assets, built);
        }
    }

    void ViewportCompositor::DriveSceneCaptures(Scene& scene, const f32 alpha, AssetManager& assets,
                                                u32& built)
    {
        ++m_CaptureSurfaceDrive.ScenesDriven;
        for (auto [entity, surface] : scene.View<CaptureSurface>())
        {
            // A disabled surface holds nothing: releasing its runtime returns the capture to the pool
            // and clears the slots it bound, as removing the component would.
            if (!surface.Enabled)
            {
                surface.Release();
                ++m_CaptureSurfaceDrive.SurfacesDisabled;
                continue;
            }

            // A capture installed this pass is registered below; one the surface already held is on
            // the drive-list.
            const bool fresh = surface.GetCapture() == nullptr;
            if (fresh && !MaterializeCapture(surface, assets, built))
            {
                ++m_CaptureSurfaceDrive.SurfacesDeferred;
                continue;
            }

            SceneCapture* const capture = surface.Drive(m_Context, assets, scene, entity, alpha);
            ++m_CaptureSurfaceDrive.SurfacesDriven;
            if (capture != nullptr && fresh)
            {
                RegisterCapture(*capture);
            }
        }
    }

    bool ViewportCompositor::MaterializeCapture(const CaptureSurface& surface, AssetManager& assets,
                                                u32& built)
    {
        const SceneCaptureInfo captureInfo = surface.GetCaptureInfo(m_Context, assets);
        Unique<SceneCapture> capture = m_CapturePool->Take(captureInfo);
        if (capture != nullptr)
        {
            ++m_CaptureSurfaceDrive.CapturesReused;
        }
        else
        {
            if (built >= MaxNewCapturesPerFrame)
            {
                return false;
            }
            VE_PROFILE_SCOPE("Capture/Materialize");
            capture = SceneCapture::Create(captureInfo);
            ++built;
            ++m_CaptureSurfaceDrive.CapturesBuilt;
        }
        surface.Materialize(m_Context, std::move(capture), m_CapturePool);
        return true;
    }

    void ViewportCompositor::DriveCaptures(CommandBuffer& cmd)
    {
        if (m_Captures.empty())
        {
            return;
        }

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 reserved = static_cast<u32>(m_Viewports.size());
        const usize count = m_Captures.size();

        // Round-robin from where the last budget-limited frame stopped, so a capture set larger than
        // the budget refreshes in turn. A capture renders one cube face per driven frame anyway, so
        // what an over-budget frame costs is refresh latency, not a capture that never renders.
        m_CaptureCursor %= count;
        usize driven = 0;
        while (driven < count)
        {
            // A capture with no fresh view records nothing and claims no slot, so the budget is
            // measured against the registry each step rather than assumed one per capture — a set of
            // settled on-demand captures displaces nothing.
            if (!CaptureDriveHasRoom(registry.GetRemainingViews(), reserved))
            {
                break;
            }
            m_Captures[CaptureDriveIndex(m_CaptureCursor, driven, count)]->Render(cmd);
            ++driven;
        }

        if (driven < count && !m_WarnedCaptureBudget)
        {
            m_WarnedCaptureBudget = true;
            Log::Warn(
                "ViewportCompositor: the frame's {} view slots cover {} viewport(s) and {} of "
                "{} scene captures; the rest hold their last map and refresh on later frames.",
                BindlessRegistry::MaxViewsPerFrame, reserved, driven, count);
        }
        m_CaptureCursor = NextCaptureCursor(m_CaptureCursor, driven, count);
    }

    void ViewportCompositor::Composite(CommandBuffer& cmd)
    {
        if (!m_Gather)
        {
            return;
        }

        vector<CompositePlacement> placements;
        for (const Viewport* viewport : m_Viewports)
        {
            if (viewport->GetRole() == ViewportRole::Presented)
            {
                placements.emplace_back(CompositePlacement{
                    .Texture = viewport->GetOutput(),
                    .Region = viewport->GetRegion(),
                });
            }
        }

        const CompositeSceneSource sceneSource =
            ResolveCompositeSceneSource(placements, m_Context.GetSwapChainExtent());
        if (sceneSource == CompositeSceneSource::Gather)
        {
            // Rebind only when the placement set changed (output identity or region), so a steady
            // frame issues no bindless re-registration.
            const auto samePlacement = [](const CompositePlacement& a, const CompositePlacement& b)
            {
                return a.Texture == b.Texture && a.Region.Offset == b.Region.Offset &&
                       a.Region.Extent == b.Region.Extent;
            };
            if (!std::ranges::equal(placements, m_GatheredPlacements, samePlacement))
            {
                m_Gather->SetPlacements(placements);
                m_GatheredPlacements = std::move(placements);
            }

            m_Gather->Execute(cmd, *m_GatherGraph);
            ApplySceneSource(m_Gather->GetOutput());
        }
        else
        {
            // An idle gather would otherwise keep the last assembled viewports' outputs alive.
            if (!m_GatheredPlacements.empty())
            {
                m_Gather->SetPlacements({});
                m_GatheredPlacements.clear();
            }
            ApplySceneSource(sceneSource == CompositeSceneSource::Direct ? placements.back().Texture
                                                                         : m_BlackView);
        }

        // The composite samples its source outside the graph that wrote it; transition it.
        cmd.PrepareForAccess(m_SceneSource, AccessKind::SampleGraphics);

        ApplyOverlaySource(ResolveCompositeOverlaySource(m_ImGui->HasDrawnOutput()) ==
                           CompositeOverlaySource::Transparent);

        m_Composite->Execute(cmd, *m_CompositeGraph, m_Context.GetCurrentSwapChainImageView());

        CompositeToSink(cmd);
    }

    void ViewportCompositor::ApplySceneSource(const Ref<ImageView>& source)
    {
        if (source == m_SceneSource)
        {
            return;
        }

        // The capture pass is built once and reused, so it is re-pointed with the presented
        // composite or it would keep sampling a source that may since have been retired.
        m_SceneSource = source;
        m_Composite->SetSceneSource(m_SceneSource);
        if (m_CaptureComposite)
        {
            m_CaptureComposite->SetSceneSource(m_SceneSource);
        }
    }

    void ViewportCompositor::ApplyOverlaySource(const bool transparent)
    {
        if (transparent == m_OverlayTransparent)
        {
            return;
        }

        m_OverlayTransparent = transparent;
        const Ref<ImageView> overlay = transparent ? m_TransparentView : nullptr;
        m_Composite->SetOverlaySource(overlay);
        if (m_CaptureComposite)
        {
            m_CaptureComposite->SetOverlaySource(overlay);
        }
    }

    void ViewportCompositor::SetCaptureSink(CaptureSink* sink)
    {
        if (sink == m_CaptureSink)
        {
            return;
        }

        if (m_CaptureRetiredHandle != 0)
        {
            m_Context.RemoveFrameRetiredCallback(m_CaptureRetiredHandle);
            m_CaptureRetiredHandle = 0;
        }

        m_CaptureSink = sink;
        ReleaseCapturePass();

        if (m_CaptureSink != nullptr)
        {
            m_CaptureRetiredHandle = m_Context.AddFrameRetiredCallback(
                [this](const u32 slot)
                {
                    if (m_CaptureSink != nullptr)
                    {
                        m_CaptureSink->OnSlotRetired(slot);
                    }
                });
        }
    }

    void ViewportCompositor::CompositeToSink(CommandBuffer& cmd)
    {
        if (m_CaptureSink == nullptr)
        {
            return;
        }

        const uvec2 presentedExtent = m_Context.GetSwapChainExtent();
        const optional<CaptureTarget> target =
            m_CaptureSink->AcquireTarget(m_Context.GetCurrentFrameInFlight(), presentedExtent);
        if (!target || !target->Image)
        {
            return;
        }

        const uvec3 targetExtent = target->Image->GetExtent();
        if (targetExtent.x != presentedExtent.x || targetExtent.y != presentedExtent.y)
        {
            // Stretching the presented frame into a differently-sized target would be a silently
            // wrong capture, so the frame is refused instead. The sink is handed the presented
            // extent, so supplying another is a contract violation rather than a race.
            VE_ASSERT(false,
                      "CaptureSink returned a {}x{} target for a {}x{} presented frame; the "
                      "capture composite writes no other extent.",
                      targetExtent.x, targetExtent.y, presentedExtent.x, presentedExtent.y);
            return;
        }

        if (!m_CaptureComposite || m_CaptureIncludesOverlay != target->IncludeOverlay)
        {
            BuildCapturePass(target->IncludeOverlay, target->Image->GetFormat(),
                             target->ColorSpace);
        }
        else if (m_CaptureFormat != target->Image->GetFormat() ||
                 m_CaptureColorSpace != target->ColorSpace)
        {
            m_CaptureFormat = target->Image->GetFormat();
            m_CaptureColorSpace = target->ColorSpace;
            m_CaptureComposite->SetSwapChainTarget(m_CaptureFormat, m_CaptureColorSpace);
        }

        Ref<ImageView>& view = m_CaptureViews[target->Image.get()];
        if (!view)
        {
            view = ImageView::Create(m_Context, {
                                                    .Name = "Capture Composite Target View",
                                                    .Image = target->Image,
                                                });
        }

        m_CaptureComposite->Execute(cmd, *m_CaptureGraph, view);
    }

    void ViewportCompositor::BuildCapturePass(const bool includeOverlay, const Format format,
                                              const DisplayColorSpace colorSpace)
    {
        VE_ASSERT(m_Gather && m_Assets, "The capture composite needs the gather + composite tail");

        m_CaptureGraph.reset();
        m_CaptureComposite = SwapChainCompositePass::Create({
            .Context = m_Context,
            .ImGui = includeOverlay ? m_ImGui : nullptr,
            .Assets = *m_Assets,
            .SceneSource = m_SceneSource,
            .SwapChainFormat = format,
            .ColorSpace = colorSpace,
        });
        if (m_OverlayTransparent)
        {
            m_CaptureComposite->SetOverlaySource(m_TransparentView);
        }

        RenderGraph graph(m_Context);
        const ResourceId targetId = graph.Import("CaptureTarget");
        m_CaptureGraph = m_CaptureComposite->Compile(graph, targetId);

        m_CaptureIncludesOverlay = includeOverlay;
        m_CaptureFormat = format;
        m_CaptureColorSpace = colorSpace;
        m_CaptureViews.clear();
    }

    void ViewportCompositor::ReleaseCapturePass()
    {
        m_CaptureGraph.reset();
        m_CaptureComposite.reset();
        m_CaptureViews.clear();
        m_CaptureFormat = Format::Undefined;
    }

    ViewportRegion ViewportCompositor::ResolveLayout(const ViewportLayout& layout) const
    {
        const vec2 renderExtent = vec2(m_Context.GetRenderExtent());
        const ivec2 offset = ivec2(glm::round(layout.Offset * renderExtent));
        const uvec2 extent = uvec2(glm::round(layout.Extent * renderExtent));
        return {.Offset = offset, .Extent = extent};
    }

    void ViewportCompositor::ResolveTrackingLayouts()
    {
        // Screen-space Gui documents lay out in logical points while the region is framebuffer
        // pixels, so a window-tracking viewport's UI scale follows the window content scale: authored
        // px render at logical size on a HiDPI display. Headless borrows no window and stamps 1.0.
        const f32 uiScale =
            m_Context.IsHeadless() ? 1.0f : m_Context.GetWindow().GetContentScale().x;

        for (Viewport* viewport : m_Viewports)
        {
            if (const optional<ViewportLayout>& layout = viewport->GetLayout())
            {
                viewport->SetRegion(ResolveLayout(*layout));
                viewport->SetUiScale(uiScale);
            }
        }
    }
}
