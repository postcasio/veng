#include <Veng/ManagedViewports.h>

#include <Veng/InputRouter.h>
#include <Veng/Log.h>
#include <Veng/WorldRunner.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneViewport.h>

#include "ManagedRebind.h"

#include <algorithm>

namespace Veng
{
    Entity ResolvePresentationSeat(const Scene& scene, const Entity boundViewer)
    {
        // The bound seat survives the rebind only when its scene-local handle still names a live Viewer
        // in the destination scene; otherwise fall to the scene's sole/first Viewer, then to no seat.
        if (!boundViewer.IsNull() && scene.IsAlive(boundViewer) && scene.Has<Viewer>(boundViewer))
        {
            return boundViewer;
        }
        for (auto [entity, viewer] : scene.View<Viewer>())
        {
            return entity;
        }
        return Entity::Null;
    }

    bool IsWorldPresentable(const WorldRunner& runner, const WorldInstanceId world)
    {
        const World* resolved = runner.ResolveWorld(world);
        if (resolved == nullptr || resolved->LiveScene == nullptr)
        {
            return false;
        }
        const SceneSimulation* sim = resolved->GetScene().GetSimulation();
        if (sim == nullptr || !sim->IsStarted())
        {
            return false;
        }
        return resolved->Pending.IsResident() && resolved->Clock.GetTick() >= 1;
    }

    ManagedViewportSet::ManagedViewportSet(Renderer::Context& context, AssetManager& assets,
                                           Renderer::ViewportCompositor& compositor,
                                           InputRouter& router, GuiDriverRegistry* const drivers)
        : m_Context(context), m_Assets(assets), m_Compositor(compositor), m_Router(router),
          m_GuiDrivers(drivers)
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

            // Hand the viewport the driver catalog so a claimed, driver-authored GuiOverlay
            // instantiates its driver on the first drive; null leaves every overlay undriven.
            viewport->SetGuiDriverRegistry(m_GuiDrivers);

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

    void ManagedViewportSet::ApplyPendingReconfigure(WorldRunner& runner, const f32 delta,
                                                     Renderer::ViewState& knobs)
    {
        if (m_PendingReconfigure)
        {
            Build(*m_PendingReconfigure);
            m_PendingReconfigure.reset();
        }

        // Rebinds apply after any reconfigure, so a rebind of a viewport the reconfigure rebuilt lands
        // on the new viewport (the same top-of-frame safe point, outside the drive loop). Each is a
        // complete rebind: detach the departed world's overlays and re-resolve the seat.
        for (const PendingRebind& rebind : m_PendingRebinds)
        {
            ApplyCompleteRebind(rebind.Index, rebind.World, runner, knobs);
        }
        m_PendingRebinds.clear();

        // Present-on-ready rebinds hold the viewport on its current world until the destination readies.
        // Drop one whose destination closed mid-wait, apply one that readied, and abandon one that
        // exceeds the timeout (surfaced through GetAbandonedPresentWorld) so a never-ready destination
        // does not strand the viewport on the old world forever.
        for (auto it = m_PendingReadyRebinds.begin(); it != m_PendingReadyRebinds.end();)
        {
            if (runner.ResolveWorld(it->World) == nullptr)
            {
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            if (IsWorldPresentable(runner, it->World))
            {
                ApplyCompleteRebind(it->Index, it->World, runner, knobs);
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            it->Waited += delta;
            if (it->Waited >= PresentReadyTimeoutSeconds)
            {
                Log::Warn("Managed viewport {} present-on-ready to world {} timed out after {}s; "
                          "abandoning and keeping the current world.",
                          it->Index, it->World.Value, it->Waited);
                m_AbandonedPresents.push_back({.Index = it->Index, .World = it->World});
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            ++it;
        }
    }

    void ManagedViewportSet::SetViewportWorld(usize index, WorldInstanceId world)
    {
        if (index < m_Viewports.size())
        {
            m_Viewports[index].Info.World = world;
        }
    }

    void ManagedViewportSet::SupersedePending(const usize index)
    {
        std::erase_if(m_PendingRebinds,
                      [index](const PendingRebind& r) { return r.Index == index; });
        std::erase_if(m_PendingReadyRebinds,
                      [index](const PendingReadyRebind& r) { return r.Index == index; });
        std::erase_if(m_AbandonedPresents,
                      [index](const AbandonedPresent& a) { return a.Index == index; });
    }

    void ManagedViewportSet::RebindWorld(const usize index, const WorldInstanceId world)
    {
        SupersedePending(index);
        m_PendingRebinds.push_back({.Index = index, .World = world});
    }

    void ManagedViewportSet::RebindWorldWhenReady(const usize index, const WorldInstanceId world)
    {
        SupersedePending(index);
        m_PendingReadyRebinds.push_back({.Index = index, .World = world});
    }

    void ManagedViewportSet::ApplyCompleteRebind(const usize index, const WorldInstanceId world,
                                                 WorldRunner& runner, Renderer::ViewState& knobs)
    {
        if (index >= m_Viewports.size())
        {
            return;
        }
        ManagedViewport& managed = m_Viewports[index];
        const WorldInstanceId departedWorld = managed.Info.World;
        const Entity departedViewer = managed.Info.Viewer;

        // Detach the departed world's engine-driven overlay documents from this viewport — the exact
        // inverse of the per-frame Drive, same frame as the rebind so no dismiss-retry window opens. A
        // closed departed world skips (its documents died with it); a rebind to the same world skips.
        if (departedWorld != world)
        {
            if (const World* departed = runner.ResolveWorld(departedWorld); departed != nullptr)
            {
                for (auto [entity, overlay] : departed->GetScene().View<GuiOverlay>())
                {
                    overlay.Detach(*managed.Viewport);
                }
            }
        }

        managed.Info.World = world;

        // Re-resolve the seat in the destination scene (a scene-local Viewer handle cannot survive a
        // scene change): the bound seat if it still resolves, else the scene's sole/first Viewer, else
        // none. Re-point the router association to it, and follow the cursor seat when the departed
        // association owned it. Focus policy (captured vs. free) is deliberately left to the game.
        Entity resolvedSeat = Entity::Null;
        if (const World* destination = runner.ResolveWorld(world); destination != nullptr)
        {
            resolvedSeat = ResolvePresentationSeat(destination->GetScene(), departedViewer);

            // The presented world's authored render settings govern the viewport that presents it —
            // the same seed the bootstrap world takes, re-applied here so a travel does not leave a
            // destination rendering under the departed level's toggles. A destination authoring no
            // LevelRenderSettings keeps the viewport's current settings (the editor and
            // engine-agnostic postures).
            if (const LevelRenderSettings* render =
                    destination->GetScene().TryGetFirst<LevelRenderSettings>())
            {
                Renderer::SceneRendererSettings settings = managed.Viewport->GetSettings();
                ApplyLevelRenderSettings(*render, settings, knobs);
                managed.Viewport->Configure(settings);
            }
        }

        // Whether the cursor seat is routed through a viewport *other* than this one, captured before
        // we reassociate. Only then must this rebind leave the cursor seat alone — a split-screen peer
        // owns it. The stored Info.Viewer is unreliable here: a viewport bound without seat resolution
        // (the bootstrap SetViewportWorld) leaves Info.Viewer null while the cursor seat still points
        // at the world this viewport presents, so the live association — not the stored viewer — is
        // the source of truth for ownership.
        const Renderer::Viewport* const cursorViewport = m_Router.ResolvePointerViewport({}, true);
        const bool cursorOwnedElsewhere =
            cursorViewport != nullptr && cursorViewport != managed.Viewport.get();

        if (resolvedSeat != Entity::Null)
        {
            m_Router.AssociateViewportSeat(*managed.Viewport, resolvedSeat);
        }
        else
        {
            m_Router.ClearViewportSeat(*managed.Viewport);
        }
        // The cursor seat follows this viewport to its new seat unless a different viewport owns it. A
        // scene-local seat handle cannot survive the scene change, so the presenting viewport's rebind
        // must move the cursor seat to the destination's seat — otherwise a captured pointer resolves
        // no viewport for the stale seat and falls back to the managed world, and the presented
        // world's seat never receives the look delta.
        if (!cursorOwnedElsewhere)
        {
            m_Router.SetCursorSeat(resolvedSeat);
        }
        managed.Info.Viewer = resolvedSeat;
    }

    WorldInstanceId ManagedViewportSet::GetViewportWorld(const usize index) const
    {
        return index < m_Viewports.size() ? m_Viewports[index].Info.World : WorldInstanceId{};
    }

    optional<WorldInstanceId> ManagedViewportSet::GetPendingViewportWorld(const usize index) const
    {
        // Supersession keeps at most one pending rebind per index across both lists, so the first match
        // is the only one.
        for (const PendingRebind& r : m_PendingRebinds)
        {
            if (r.Index == index)
            {
                return r.World;
            }
        }
        for (const PendingReadyRebind& r : m_PendingReadyRebinds)
        {
            if (r.Index == index)
            {
                return r.World;
            }
        }
        return std::nullopt;
    }

    WorldInstanceId ManagedViewportSet::GetAbandonedPresentWorld(const usize index) const
    {
        for (const AbandonedPresent& a : m_AbandonedPresents)
        {
            if (a.Index == index)
            {
                return a.World;
            }
        }
        return WorldInstanceId{};
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
