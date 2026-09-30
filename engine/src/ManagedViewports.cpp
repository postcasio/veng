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
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/LocalControl.h>
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
        // in the destination scene; otherwise the first locally-owned Viewer, else no seat. A viewport
        // presents this peer's own seat: a host scene whose only seats belong to remote peers resolves
        // none, and the viewport is seated once its own seat exists (ResolveUnboundSeats). With no
        // marker and no remote owner every seat is locally owned, so a single-seat scene resolves its
        // one Viewer.
        if (!boundViewer.IsNull() && scene.IsAlive(boundViewer) && scene.Has<Viewer>(boundViewer))
        {
            return boundViewer;
        }
        for (auto [entity, viewer] : scene.View<Viewer>())
        {
            if (IsLocallyOwned(scene, entity))
            {
                return entity;
            }
        }
        return Entity::Null;
    }

    namespace
    {
        // The sharpest scale the viewport is allocated for: the controller's ceiling where it owns
        // the scale, else the static scale (which is its own ceiling).
        f32 RenderScaleCeiling(const Renderer::Viewport& viewport)
        {
            const optional<Renderer::DynamicResolutionSettings>& settings =
                viewport.GetDynamicResolution();
            return settings ? settings->MaxScale : viewport.GetRenderScale();
        }
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

    bool IsWorldPresentable(const WorldRunner& runner, const WorldInstanceId world,
                            const WorldPresentReadyGate& gate)
    {
        if (!IsWorldPresentable(runner, world))
        {
            return false;
        }
        // The gate runs only past the engine's own test, so a consumer predicate may read the
        // destination's scene without checking it is installed.
        return !gate || gate(*runner.ResolveWorld(world));
    }

    ManagedViewportSet::ManagedViewportSet(Renderer::Context& context, AssetManager& assets,
                                           Renderer::ViewportCompositor& compositor,
                                           InputRouter& router, GuiDriverRegistry* const drivers,
                                           Audio::AudioEngine* const audio,
                                           const Gui::GuiTranslator* const translator)
        : m_Context(context), m_Assets(assets), m_Compositor(compositor), m_Router(router),
          m_GuiDrivers(drivers), m_Audio(audio), m_GuiTranslator(translator)
    {
    }

    ManagedViewportSet::~ManagedViewportSet()
    {
        Clear();
    }

    void ManagedViewportSet::SetLocalization(const Localization::Localization* const localization)
    {
        m_Localization = localization;
        for (ManagedViewport& managed : m_Viewports)
        {
            managed.Viewport->SetLocalization(localization);
        }
        // A store swap (a new service pointer, not just a locale generation bump) must reach the
        // bound viewports too, since RegisterBoundViewport captured the pointer that was current then.
        for (const BoundViewport& bound : m_Bound)
        {
            bound.Viewport->SetLocalization(localization);
        }
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

            // A viewport built while the set is held joins the hold at construction, so a world
            // transition rebuilding the set does not let the controller back in mid-hold.
            if (m_RenderScaleHeld)
            {
                viewport->HoldRenderScale(RenderScaleCeiling(*viewport));
            }

            // Hand the viewport the driver catalog so a claimed, driver-authored GuiOverlay
            // instantiates its driver on the first drive; null leaves every overlay undriven.
            viewport->SetGuiDriverRegistry(m_GuiDrivers);
            // And the audio engine its drivers fire sound through; null hands them a silent frame.
            viewport->SetAudioEngine(m_Audio);
            // And the translator its overlay documents localize markup loc-keys through; null renders keys.
            viewport->SetGuiTranslator(m_GuiTranslator);
            // And the service its drivers compose runtime strings through; null hands the null-object.
            viewport->SetLocalization(m_Localization);

            m_Compositor.RegisterViewport(*viewport);

            // A managed viewport bound to a seat feeds that seat's pointer input: associate it with
            // the router in the same step it is registered, so a free cursor over its region routes
            // to the seat with no dead frame. An unbound viewport (the default single-camera path)
            // needs no association — under capture the pointer routes to the keyboard seat directly.
            if (info.Viewer != Entity::Null)
            {
                m_Router.AssociateViewportSeat(*viewport,
                                               SeatRef{.World = info.World, .Viewer = info.Viewer});
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
        // Apply one that readied, and abandon one whose destination vanished mid-wait or that exceeds
        // the timeout (both surfaced through GetAbandonedPresentWorld) so a never-ready or reaped
        // destination does not strand the viewport on the old world forever.
        for (auto it = m_PendingReadyRebinds.begin(); it != m_PendingReadyRebinds.end();)
        {
            if (runner.ResolveWorld(it->World) == nullptr)
            {
                // The destination vanished while still waiting — idle-reaped or closed out from under
                // the wait. A deliberate supersession never reaches here: a later rebind of this index
                // erases the pending through SupersedePending before this drive runs, so a pending
                // still in the list whose world no longer resolves genuinely disappeared. Record the
                // abandonment (as the timeout path does) so every non-completion is observable.
                Log::Warn("Managed viewport {} present-on-ready to world {} was abandoned: its "
                          "destination closed before it became ready; keeping the current world.",
                          it->Index, it->World.Value);
                m_AbandonedPresents.push_back({.Index = it->Index, .World = it->World});
                m_AbandonedEvents.push_back({.Index = it->Index, .World = it->World});
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            if (IsWorldPresentable(runner, it->World, m_PresentReadyGate))
            {
                ApplyCompleteRebind(it->Index, it->World, runner, knobs);
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            it->Waited += delta;
            if (it->Waited >= PresentReadyTimeoutSeconds)
            {
                // A timed-out wait retries before it abandons: the common cause is a transient
                // stall that clears on a later attempt, so the clock restarts up to the attempt
                // budget and only a persistently un-ready destination surfaces as abandoned.
                if (it->Attempts + 1 < PresentReadyAttempts)
                {
                    ++it->Attempts;
                    it->Waited = 0.0f;
                    Log::Warn("Managed viewport {} present-on-ready to world {} has not readied "
                              "within {}s; retrying the wait (attempt {} of {}).",
                              it->Index, it->World.Value, PresentReadyTimeoutSeconds,
                              it->Attempts + 1, PresentReadyAttempts);
                    ++it;
                    continue;
                }
                Log::Warn("Managed viewport {} present-on-ready to world {} stayed unready across "
                          "{} attempts of {}s; abandoning and keeping the current world.",
                          it->Index, it->World.Value, PresentReadyAttempts,
                          PresentReadyTimeoutSeconds);
                m_AbandonedPresents.push_back({.Index = it->Index, .World = it->World});
                m_AbandonedEvents.push_back({.Index = it->Index, .World = it->World});
                it = m_PendingReadyRebinds.erase(it);
                continue;
            }
            ++it;
        }

        // Last, so a viewport a rebind just resolved a seat for is already bound and skipped: give a
        // seat to any viewport still without one. Running at this top-of-frame point puts it ahead of
        // the request drain, so a seat resolved here is the one a focus request stamped at simulation
        // start reconciles against on this very frame rather than a frame later.
        ResolveUnboundSeats(runner);

        // Stamp each completed rebind's seat only now: a rebind that resolved none leaves its
        // viewport for the pass above, which may seat it on this very frame, and what a consumer
        // wants is the seat the frame ends with.
        for (PresentedViewport& presented : m_PresentedEvents)
        {
            presented.Seat = GetViewportViewer(presented.Index);
        }
    }

    void ManagedViewportSet::DrainPresentedViewports(vector<PresentedViewport>& out)
    {
        out.clear();
        out.swap(m_PresentedEvents);
    }

    void ManagedViewportSet::DrainAbandonedPresents(vector<AbandonedPresent>& out)
    {
        out.clear();
        out.swap(m_AbandonedEvents);
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

    void ManagedViewportSet::SetPresentReadyGate(WorldPresentReadyGate gate)
    {
        m_PresentReadyGate = std::move(gate);
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

        AdoptViewportSeat(managed, resolvedSeat);

        // The viewport↔seat binding is a derivation point of the locally-controlled marker, and it
        // just moved: reconcile both ends here rather than leaving the departed world carrying a
        // marker for a seat nothing presents any more. The departed world keeps the markers of any
        // split-screen peer still presenting it.
        vector<Entity> seats;
        if (departedWorld != world)
        {
            if (const World* departed = runner.ResolveWorld(departedWorld);
                departed != nullptr && departed->LiveScene != nullptr)
            {
                CollectPresentingSeats(departedWorld, seats);
                ReconcileLocalControl(departed->GetScene(), seats);
            }
        }
        if (const World* destination = runner.ResolveWorld(world);
            destination != nullptr && destination->LiveScene != nullptr)
        {
            CollectPresentingSeats(world, seats);
            ReconcileLocalControl(destination->GetScene(), seats);
        }

        // The seat is filled in once the whole pass has run (see ApplyPendingReconfigure).
        m_PresentedEvents.push_back({.Index = index, .World = world});
    }

    void ManagedViewportSet::AdoptViewportSeat(ManagedViewport& managed, const Entity seat)
    {
        // Whether the cursor seat is routed through a viewport *other* than this one, captured before
        // we reassociate. Only then must this leave the cursor seat alone — a split-screen peer owns
        // it. The stored Info.Viewer is unreliable here: a viewport bound without seat resolution (the
        // bootstrap SetViewportWorld) leaves Info.Viewer null while the cursor seat still points at the
        // world this viewport presents, so the live association — not the stored viewer — is the source
        // of truth for ownership.
        const Renderer::Viewport* const cursorViewport = m_Router.ResolvePointerViewport({}, true);
        const bool cursorOwnedElsewhere =
            cursorViewport != nullptr && cursorViewport != managed.Viewport.get();

        const SeatRef adopted{.World = managed.Info.World, .Viewer = seat};
        if (seat != Entity::Null)
        {
            m_Router.AssociateViewportSeat(*managed.Viewport, adopted);
        }
        else
        {
            m_Router.ClearViewportSeat(*managed.Viewport);
        }
        // The cursor seat follows this viewport to its new seat unless a different viewport owns it,
        // or a captured pointer resolves no viewport for the stale seat and the presented world's seat
        // never receives the look delta. It carries its focus along: the user holding the cursor has
        // not changed, so a capture they hold must not lapse until the new world re-requests it.
        if (!cursorOwnedElsewhere)
        {
            m_Router.MoveCursorSeat(adopted);
        }
        managed.Info.Viewer = seat;
    }

    void ManagedViewportSet::ResolveUnboundSeats(WorldRunner& runner)
    {
        for (ManagedViewport& managed : m_Viewports)
        {
            if (!managed.Info.Viewer.IsNull() || !managed.Info.World.IsValid())
            {
                continue;
            }
            const World* const presented = runner.ResolveWorld(managed.Info.World);
            if (presented == nullptr || presented->LiveScene == nullptr)
            {
                continue;
            }
            // No bound viewer to preserve — this viewport has none, which is why it is being resolved.
            if (const Entity seat = ResolvePresentationSeat(presented->GetScene(), Entity::Null);
                seat != Entity::Null)
            {
                AdoptViewportSeat(managed, seat);
            }
        }
    }

    WorldInstanceId ManagedViewportSet::GetViewportWorld(const usize index) const
    {
        return index < m_Viewports.size() ? m_Viewports[index].Info.World : WorldInstanceId{};
    }

    Entity ManagedViewportSet::GetViewportViewer(const usize index) const
    {
        return index < m_Viewports.size() ? m_Viewports[index].Info.Viewer : Entity::Null;
    }

    void ManagedViewportSet::CollectPresentingSeats(const WorldInstanceId world,
                                                    vector<Entity>& seats) const
    {
        seats.clear();
        for (const ManagedViewport& managed : m_Viewports)
        {
            if (managed.Info.World == world && !managed.Info.Viewer.IsNull())
            {
                seats.push_back(managed.Info.Viewer);
            }
        }
    }

    bool ManagedViewportSet::IsWorldPresented(const WorldInstanceId world) const
    {
        if (!world.IsValid())
        {
            return false;
        }

        for (const ManagedViewport& managed : m_Viewports)
        {
            if (managed.Info.World == world)
            {
                return true;
            }
        }
        for (const BoundViewport& bound : m_Bound)
        {
            if (bound.World == world)
            {
                return true;
            }
        }

        // A rebind's destination is presented for the whole in-flight window, including a
        // present-on-ready wait that spans many frames: the swap happens in one frame, so the
        // destination's presentation-gated work has to already be warm when it does.
        const auto destinedFor = [world](const auto& pending) { return pending.World == world; };
        return std::ranges::any_of(m_PendingRebinds, destinedFor) ||
               std::ranges::any_of(m_PendingReadyRebinds, destinedFor);
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
        ResolveDofViewState(state, static_cast<f32>(output->GetImage()->GetHeight()));
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
        // A bound viewport is created by its caller (a LevelOverlay), not by this set, so it has none
        // of the engine services a set-created viewport is handed in ReconfigureManagedViewports.
        // Wire the same four the set owns, so an engine-driven GuiOverlay/GuiSurface presented here
        // instantiates its driver and localizes its markup exactly as one on a managed viewport does;
        // without them the overlay renders raw loc-keys, plays no sound, and never drives its driver.
        viewport.SetGuiDriverRegistry(m_GuiDrivers);
        viewport.SetAudioEngine(m_Audio);
        viewport.SetGuiTranslator(m_GuiTranslator);
        viewport.SetLocalization(m_Localization);

        m_Bound.push_back(
            {.Viewport = &viewport, .World = world, .Viewer = viewer, .Knobs = knobs});
    }

    void ManagedViewportSet::UnregisterBoundViewport(const Renderer::Viewport& viewport)
    {
        std::erase_if(m_Bound, [&viewport](const BoundViewport& bound)
                      { return bound.Viewport == &viewport; });
    }

    void ManagedViewportSet::SetRenderScaleHold(const bool held)
    {
        m_RenderScaleHeld = held;

        for (const ManagedViewport& managed : m_Viewports)
        {
            if (held)
            {
                managed.Viewport->HoldRenderScale(RenderScaleCeiling(*managed.Viewport));
            }
            else
            {
                managed.Viewport->ReleaseRenderScale();
            }
        }
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
