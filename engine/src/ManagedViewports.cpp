#include <Veng/ManagedViewports.h>

#include <Veng/InputRouter.h>
#include <Veng/WorldRunner.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneViewport.h>

namespace Veng
{
    ManagedViewportSet::ManagedViewportSet(Renderer::Context& context, AssetManager& assets,
                                           Renderer::ViewportCompositor& compositor,
                                           InputRouter& router)
        : m_Context(context), m_Assets(assets), m_Compositor(compositor), m_Router(router)
    {
    }

    ManagedViewportSet::~ManagedViewportSet()
    {
        Clear();
    }

    Renderer::Viewport* ManagedViewportSet::Get(usize index) const
    {
        return index < m_Viewports.size() ? m_Viewports[index].Viewport.get() : nullptr;
    }

    void ManagedViewportSet::Build(std::span<const ManagedViewportInfo> infos)
    {
        // Drop the prior set first (each Unique self-unregisters from the compositor drive-list),
        // clearing each one's router association so no stale pointer lingers. Then build the new set
        // in order so index 0 is the primary.
        Clear();
        m_Viewports.reserve(infos.size());

        for (const ManagedViewportInfo& info : infos)
        {
            // A pinned Extent is a fixed render resolution at the origin; otherwise the region is the
            // Layout resolved against the render extent, and the viewport tracks the window.
            const bool tracksWindow = info.Extent == uvec2{};
            const Renderer::ViewportRegion region =
                tracksWindow ? m_Compositor.ResolveLayout(info.Layout)
                             : Renderer::ViewportRegion{.Offset = {0, 0}, .Extent = info.Extent};

            Unique<Renderer::Viewport> viewport = Renderer::Viewport::Create({
                .Context = m_Context,
                .Assets = m_Assets,
                .Region = region,
                .ColorFormat = info.ColorFormat,
                .Settings = info.Settings,
                .RenderScale = info.RenderScale,
                .MaxAllocationScale = info.MaxAllocationScale,
                .Role = Renderer::ViewportRole::Presented,
            });

            // A window-tracking viewport carries its Layout so the compositor re-resolves its region
            // and UI scale on resize; a pinned one keeps its absolute region.
            if (tracksWindow)
            {
                viewport->SetLayout(info.Layout);
            }

            // Opt-in adaptive resolution: the viewport drives its own per-frame sub-rect scale from
            // GPU frame time over the fixed allocation.
            if (info.DynamicResolution)
            {
                viewport->SetDynamicResolution(*info.DynamicResolution);
            }

            m_Compositor.RegisterViewport(*viewport);

            // A managed viewport bound to a seat feeds that seat's pointer input: associate it with
            // the router in the same step it is registered, so a free cursor over its region routes
            // to the seat with no dead frame. An unbound viewport (the default single-camera path)
            // needs no association — under capture the pointer routes to the keyboard seat directly.
            if (info.Viewer != Entity::Null)
            {
                m_Router.AssociateViewportSeat(*viewport, info.Viewer);
            }

            m_Viewports.push_back({.Viewport = std::move(viewport), .Info = info});
        }

        // Stamp each window-tracking viewport's region and UI scale from the current window: the
        // single layout-resolution path the resize reaction also runs, so no per-frame re-apply.
        m_Compositor.ResolveTrackingLayouts();
    }

    void ManagedViewportSet::Reconfigure(std::span<const ManagedViewportInfo> infos)
    {
        VE_ASSERT(!m_Viewports.empty(),
                  "ManagedViewportSet::Reconfigure requires a managed viewport built at startup");

        m_PendingReconfigure = vector<ManagedViewportInfo>(infos.begin(), infos.end());
    }

    void ManagedViewportSet::ApplyPendingReconfigure()
    {
        if (m_PendingReconfigure)
        {
            Build(*m_PendingReconfigure);
            m_PendingReconfigure.reset();
        }
    }

    void ManagedViewportSet::SetViewportWorld(usize index, WorldInstanceId world)
    {
        if (index < m_Viewports.size())
        {
            m_Viewports[index].Info.World = world;
        }
    }

    void ManagedViewportSet::PushViewportView(Renderer::Viewport& viewport, WorldInstanceId world,
                                              Entity viewer, WorldRunner& runner,
                                              const Renderer::ViewState& knobs, f32 delta,
                                              f32 alpha) const
    {
        // An invalid World is a game-driven viewport: the engine pushes nothing, so the game's own
        // SetViewState stands.
        if (!world.IsValid())
        {
            return;
        }

        // A world closed at runtime resolves to nothing: push a null-scene ViewState so the viewport
        // drops its retained scene pointer and renders a cleared target (inert, never a dangling read).
        const World* resolved = runner.ResolveWorld(world);
        if (resolved == nullptr)
        {
            Renderer::ViewState cleared = knobs;
            cleared.World = nullptr;
            cleared.Delta = delta;
            cleared.Alpha = alpha;
            viewport.SetViewState(cleared);
            return;
        }

        const Scene& scene = resolved->GetScene();

        // A viewport with no bound Viewer takes the world's scene primary camera — the delivered
        // single-viewport path, byte-identical for the default managed viewport.
        if (viewer == Entity::Null)
        {
            PushSceneView(viewport, scene, knobs, delta, alpha);
            return;
        }

        // A bound Viewer resolves that seat's camera at the viewport's aspect, falling back to the
        // default framing when the seat resolves none (mirrors PushSceneView's fallback).
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());

        Renderer::ViewState state = knobs;
        state.World = &scene;
        state.Camera =
            runner.ResolveCameraView(world, viewer, aspect).value_or(DefaultCameraView(aspect));
        state.Delta = delta;
        state.Alpha = alpha;
        viewport.SetViewState(state);
    }

    void ManagedViewportSet::PushViews(WorldRunner& runner, const Renderer::ViewState& knobs,
                                       f32 delta, f32 alpha)
    {
        for (const ManagedViewport& managed : m_Viewports)
        {
            PushViewportView(*managed.Viewport, managed.Info.World, managed.Info.Viewer, runner,
                             knobs, delta, alpha);
        }

        // Bound viewports (overlays) carry their own knobs and present a world other than the one
        // driving the frame's alpha, so each reads its world's own interpolation fraction.
        for (const BoundViewport& bound : m_Bound)
        {
            PushViewportView(*bound.Viewport, bound.World, bound.Viewer, runner, bound.Knobs, delta,
                             runner.ResolveAlpha(bound.World));
        }
    }

    void ManagedViewportSet::RegisterBoundViewport(Renderer::Viewport& viewport,
                                                   WorldInstanceId world, Entity viewer,
                                                   const Renderer::ViewState& knobs)
    {
        m_Bound.push_back(
            {.Viewport = &viewport, .World = world, .Viewer = viewer, .Knobs = knobs});
    }

    void ManagedViewportSet::UnregisterBoundViewport(const Renderer::Viewport& viewport)
    {
        std::erase_if(m_Bound, [&viewport](const BoundViewport& bound)
                      { return bound.Viewport == &viewport; });
    }

    void ManagedViewportSet::Clear()
    {
        for (const ManagedViewport& managed : m_Viewports)
        {
            m_Router.ClearViewportSeat(*managed.Viewport);
        }
        m_Viewports.clear();
    }
}
