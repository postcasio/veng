#include "OverlayWorld.h"

#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/LevelOverlay.h>
#include <Veng/Log.h>
#include <Veng/ManagedViewports.h>
#include <Veng/Window.h>
#include <Veng/World.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>
#include <Veng/Task/TaskSystem.h>

#include <algorithm>
#include <utility>

namespace Veng
{
    namespace
    {
        // An engine-owned empty input-mapping context the suspend scope swaps the seat beneath's
        // input to. It carries a sentinel id so the focus scope treats it as a real swap-in (the
        // scope swaps only for a valid-id context) and resolves no actions, so the suspended seat
        // goes neutral while the overlay is up. Each overlay owns its own, released on close.
        AssetHandle<InputMappingContext> MakeSuspendContext()
        {
            constexpr AssetId SuspendContextId{.Value = 0x181FA3B5C5BA27EFULL};
            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = SuspendContextId,
                .Type = AssetTypeTrait<InputMappingContext>::Type,
                .Resource = std::static_pointer_cast<void>(InputMappingContext::Create({}, {})),
            });
            AssetHandle<InputMappingContext> handle;
            Detail::RehydrateHandleField(&handle, SuspendContextId, std::move(entry));
            return handle;
        }

        // The scene the seat lives in, resolved through its world. Null for the implicit seat or a
        // world that no longer resolves, which makes the suspend scope's context swap an inert no-op.
        Scene* SceneOfSeat(WorldRunner& runner, const SeatRef seat)
        {
            if (seat.IsImplicit())
            {
                return nullptr;
            }
            World* const world = runner.ResolveWorld(seat.World);
            return world != nullptr ? &world->GetScene() : nullptr;
        }

        // Copies the seed's reflected components into one fresh entity of the overlay scene. The
        // hierarchy and the request itself stay behind: the first names this scene's entities, the
        // second would open the overlay again from inside itself.
        void SeedOverlay(Scene& opener, const Entity seed, Scene& overlay)
        {
            const TypeRegistry& types = opener.GetTypeRegistry();
            const Entity target = overlay.CreateEntity();
            const EntityRemap cleared = [](Entity) { return Entity::Null; };
            vector<u8> scratch;
            opener.ForEachComponent(seed,
                                    [&](const TypeId id, void* component)
                                    {
                                        const TypeInfo& info = types.Info(id);
                                        if (info.Fields.empty() || id == TypeIdOf<Hierarchy>() ||
                                            id == TypeIdOf<LevelOverlay>())
                                        {
                                            return;
                                        }
                                        CopyComponentFields(component,
                                                            overlay.AddComponent(target, id), info,
                                                            types, cleared, scratch);
                                    });
        }

        // Removes T from an entity that carries it.
        template <class T>
        void RemoveIfPresent(Scene& scene, const Entity entity)
        {
            if (std::as_const(scene).TryGet<T>(entity) != nullptr)
            {
                (void)scene.Remove<T>(entity);
            }
        }
    }

    OverlayWorld::~OverlayWorld() = default;

    OverlayWorlds::OverlayWorlds(OverlayServices services) : m_Services(std::move(services)) {}

    OverlayWorlds::~OverlayWorlds()
    {
        // Every world is closed before the application's members drop, and each close took its
        // overlays with it, so this unwinds only after a run that never reached that sweep. The
        // worlds are left to the runner: stopping them needs services already being destroyed.
        while (!m_Open.empty())
        {
            Close(m_Open.back().get(), false, false);
        }
    }

    void OverlayWorlds::Reconcile()
    {
        WorldRunner& runner = m_Services.Runner;

        // Closes first, newest first, so the closes of one frame unwind in reverse open order.
        for (usize i = m_Open.size(); i-- > 0;)
        {
            // A close also closes the overlays opened from inside it, all of them newer.
            if (i >= m_Open.size())
            {
                continue;
            }
            OverlayWorld* const overlay = m_Open[i].get();
            if (runner.ResolveWorld(overlay->World) == nullptr)
            {
                Close(overlay, false, true);
                continue;
            }
            Scene* const opener = ResolveOpenerScene(*overlay);
            if (opener == nullptr || !opener->IsAlive(overlay->Opener) ||
                std::as_const(*opener).TryGet<LevelOverlay>(overlay->Opener) == nullptr)
            {
                Close(overlay, true, false);
            }
        }

        // Gathered before any opens, since an open adds a world to the list walked here.
        vector<std::pair<WorldInstanceId, Entity>> requested;
        for (const Unique<World>& world : runner.GetWorlds())
        {
            const WorldRequestPolicy* const policy = m_Services.Policy(world->Id);
            if (world->LiveScene == nullptr ||
                (policy != nullptr && policy->Mode == WorldRequestMode::Sandboxed))
            {
                continue;
            }
            for (auto [entity, request] : world->GetScene().View<LevelOverlay>())
            {
                const bool open = std::ranges::any_of(
                    m_Open, [&](const Unique<OverlayWorld>& overlay)
                    { return overlay->OpenerWorld == world->Id && overlay->Opener == entity; });
                if (!open)
                {
                    requested.emplace_back(world->Id, entity);
                }
            }
        }
        for (const auto& [world, entity] : requested)
        {
            TryOpen(world, entity);
        }

        ApplyOpaque();
    }

    void OverlayWorlds::OnWorldClosed(const WorldInstanceId world)
    {
        // The overlays this world opened, newest first.
        for (;;)
        {
            const auto it = std::ranges::find_if(m_Open.rbegin(), m_Open.rend(),
                                                 [world](const Unique<OverlayWorld>& overlay)
                                                 { return overlay->OpenerWorld == world; });
            if (it == m_Open.rend())
            {
                break;
            }
            Close(it->get(), true, false);
        }

        // The overlay this world was: it ended itself, or something else closed it.
        const auto it = std::ranges::find_if(m_Open, [world](const Unique<OverlayWorld>& overlay)
                                             { return overlay->World == world; });
        if (it != m_Open.end())
        {
            Close(it->get(), false, true);
        }

        ApplyOpaque();
    }

    Renderer::Viewport* OverlayWorlds::FindViewport(const WorldInstanceId world) const
    {
        const auto it = std::ranges::find_if(m_Open, [world](const Unique<OverlayWorld>& overlay)
                                             { return overlay->World == world; });
        return it != m_Open.end() ? (*it)->Viewport.get() : nullptr;
    }

    void OverlayWorlds::TryOpen(const WorldInstanceId world, const Entity entity)
    {
        World* const opener = m_Services.Runner.ResolveWorld(world);
        if (opener == nullptr)
        {
            return;
        }
        Scene& scene = opener->GetScene();
        auto* const request = scene.TryGet<LevelOverlay>(entity);
        if (request == nullptr)
        {
            return;
        }

        AssetHandle<Level>& source = request->Source;
        if (!source.IsLoaded())
        {
            const AssetId id = source.Id();
            if (source.HasFailed() || !id.IsValid())
            {
                Log::Error("LevelOverlay: level {:#018x} could not be opened; removing the request",
                           id.Value);
                (void)scene.Remove<LevelOverlay>(entity);
                return;
            }
            if (request->WaitForResidency)
            {
                const AssetResult<AssetHandle<Level>> loaded =
                    m_Services.Assets.LoadSync<Level>(id);
                if (!loaded.has_value())
                {
                    Log::Error("LevelOverlay: level {:#018x} failed to load: {}", id.Value,
                               loaded.error().Detail);
                    (void)scene.Remove<LevelOverlay>(entity);
                    return;
                }
                source = *loaded;
            }
            else
            {
                // The component holds the load, and the open is retried each frame until it lands.
                if (!source.IsValid())
                {
                    source = m_Services.Assets.Load<Level>(id);
                    if (!source.IsValid())
                    {
                        Log::Error("LevelOverlay: level {:#018x} does not resolve; removing the "
                                   "request",
                                   id.Value);
                        (void)scene.Remove<LevelOverlay>(entity);
                    }
                }
                return;
            }
        }

        // Read once: an edit to a live request does nothing until it is removed and re-added.
        const LevelOverlay copy = *request;
        Open(world, scene, entity, copy);
    }

    void OverlayWorlds::Open(const WorldInstanceId openerId, Scene& openerScene,
                             const Entity entity, const LevelOverlay& request)
    {
        WorldRunner& runner = m_Services.Runner;
        InputRouter& router = m_Services.Router;

        // Opened unstarted, so the seed and the application's hook land before any OnStart.
        const WorldInstanceId overlayId = runner.OpenWorld(WorldOpenInfo{
            .Source = request.Source,
            .StartSimulation = false,
            .OnLoaded =
                [&](const WorldInstanceId id, Scene& scene, ResidencyBatch&)
            {
                if (!request.Seed.IsNull() && openerScene.IsAlive(request.Seed))
                {
                    SeedOverlay(openerScene, request.Seed, scene);
                }
                if (m_Services.Loaded)
                {
                    m_Services.Loaded(openerId, entity, id, scene);
                }
            },
        });

        World& world = *runner.ResolveWorld(overlayId);
        if (request.WaitForResidency)
        {
            world.Pending.WaitResident(m_Services.Tasks);
        }
        Scene& scene = world.GetScene();

        auto overlay = CreateUnique<OverlayWorld>();
        overlay->OpenerWorld = openerId;
        overlay->Opener = entity;
        overlay->OpenerScene = &openerScene;
        overlay->World = overlayId;
        overlay->Opaque = request.Opaque;

        // Registered last, so it composites over every viewport registered before it.
        Renderer::Context& context = m_Services.Context;
        overlay->Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = m_Services.Assets,
            .Region = m_Services.Compositor.ResolveLayout(request.Layout),
            .Role = Renderer::ViewportRole::Presented,
            // Screen-space Gui documents lay out in logical points, so the overlay's HUD renders at
            // logical size on a HiDPI display (the compositor re-stamps it on resize).
            .UiScale = context.IsHeadless() ? 1.0f : context.GetWindow().GetContentScale().x,
        });
        overlay->Viewport->SetLayout(request.Layout);
        m_Services.Compositor.RegisterViewport(*overlay->Viewport);

        const InputSeat seat = ResolveInputSeat(&scene, overlayId);
        overlay->Seat = seat.GetRef();
        m_Services.Managed.RegisterBoundViewport(
            *overlay->Viewport,
            BoundViewportInfo{.World = overlayId, .Viewer = overlay->Seat.Viewer});
        // The seat's roles dispatch only to viewports bound to it, so the overlay's documents
        // navigate from its own input contexts.
        overlay->Viewport->SetSeat(overlay->Seat);

        router.AssociateViewportSeat(*overlay->Viewport, overlay->Seat);
        overlay->PriorCursorSeat = router.GetCursorSeat();
        router.SetCursorSeat(overlay->Seat);

        overlay->SuspendedSeat = !request.SuspendSeat.IsNull()
                                     ? SeatRef{.World = openerId, .Viewer = request.SuspendSeat}
                                     : overlay->PriorCursorSeat;
        overlay->SuspendContext = MakeSuspendContext();
        const InputSeat suspend{.Viewer = overlay->SuspendedSeat.Viewer,
                                .World = SceneOfSeat(runner, overlay->SuspendedSeat),
                                .WorldId = overlay->SuspendedSeat.World};
        overlay->Suspend =
            CreateUnique<SeatFocusScope>(router, suspend, nullptr, overlay->SuspendContext);

        if (request.PauseOpener)
        {
            overlay->Pause = runner.PauseScope(openerId);
        }

        // The overlay world's ExitRequest ends the overlay rather than the application; the close
        // reaches the world-closed hook, which unwinds it and removes the request.
        m_Services.SetPolicy(overlayId,
                             WorldRequestPolicy{.Mode = WorldRequestMode::Full,
                                                .OnExit = [&runner](const WorldInstanceId closing)
                                                { runner.CloseWorld(closing); }});

        const LevelOverlayState state{.World = overlayId, .Seat = overlay->Seat};
        if (auto* const held = openerScene.TryGet<LevelOverlayState>(entity))
        {
            *held = state;
        }
        else
        {
            openerScene.Add<LevelOverlayState>(entity, state);
        }

        m_Open.push_back(std::move(overlay));
        runner.StartWorld(overlayId);
    }

    void OverlayWorlds::Close(OverlayWorld* const target, const bool closeWorld,
                              const bool removeRequest)
    {
        // The overlays opened from inside this one close first, newest first.
        for (;;)
        {
            const auto dependent = std::ranges::find_if(
                m_Open.rbegin(), m_Open.rend(), [target](const Unique<OverlayWorld>& overlay)
                { return overlay->OpenerWorld == target->World; });
            if (dependent == m_Open.rend())
            {
                break;
            }
            Close(dependent->get(), true, false);
        }

        const auto it = std::ranges::find_if(m_Open, [target](const Unique<OverlayWorld>& open)
                                             { return open.get() == target; });
        if (it == m_Open.end())
        {
            return;
        }
        const Unique<OverlayWorld> overlay = std::move(*it);
        m_Open.erase(it);

        const bool restoreCursor = Unlink(*overlay);
        Unwind(*overlay, closeWorld, restoreCursor);

        // Resolved after the world closes: an OnStop may have closed or restructured the opener.
        if (Scene* const opener = ResolveOpenerScene(*overlay);
            opener != nullptr && opener->IsAlive(overlay->Opener))
        {
            RemoveIfPresent<LevelOverlayState>(*opener, overlay->Opener);
            if (removeRequest)
            {
                RemoveIfPresent<LevelOverlay>(*opener, overlay->Opener);
            }
        }
    }

    bool OverlayWorlds::Unlink(OverlayWorld& overlay)
    {
        bool restoreCursor = true;
        // An overlay opened above this one took its cursor seat and suspended its seat. Closing
        // this one out of order hands both on: the one above restores what this one would have,
        // and suspends what this one suspended, so neither surfaces while it is still open.
        for (const Unique<OverlayWorld>& above : m_Open)
        {
            if (above->PriorCursorSeat == overlay.Seat)
            {
                above->PriorCursorSeat = overlay.PriorCursorSeat;
                restoreCursor = false;
            }
            if (above->SuspendedSeat == overlay.Seat)
            {
                std::swap(above->Suspend, overlay.Suspend);
                std::swap(above->SuspendContext, overlay.SuspendContext);
                std::swap(above->SuspendedSeat, overlay.SuspendedSeat);
            }
        }
        return restoreCursor;
    }

    void OverlayWorlds::Unwind(OverlayWorld& overlay, const bool closeWorld,
                               const bool restoreCursor)
    {
        InputRouter& router = m_Services.Router;

        overlay.Pause = WorldPauseScope{};

        // Safe when the suspended seat's world has closed: the router retired the token with it.
        overlay.Suspend.reset();

        if (restoreCursor)
        {
            router.SetCursorSeat(overlay.PriorCursorSeat);
        }
        if (overlay.Viewport)
        {
            router.ClearViewportSeat(overlay.Viewport->GetId());
            m_Services.Managed.UnregisterBoundViewport(*overlay.Viewport);
        }
        overlay.Viewport.reset();

        if (closeWorld && m_Services.Runner.ResolveWorld(overlay.World) != nullptr)
        {
            m_Services.Runner.CloseWorld(overlay.World);
        }
    }

    void OverlayWorlds::ApplyOpaque()
    {
        const bool anyOpaque = std::ranges::any_of(m_Open, [](const Unique<OverlayWorld>& overlay)
                                                   { return overlay->Opaque; });
        if (!anyOpaque && m_Disabled.empty())
        {
            return;
        }

        vector<Renderer::ViewportId> covered;
        for (const Unique<OverlayWorld>& overlay : m_Open)
        {
            if (!overlay->Opaque)
            {
                continue;
            }
            m_Services.Managed.CollectWorldViewports(overlay->OpenerWorld, m_Scratch);
            for (const Renderer::Viewport* viewport : m_Scratch)
            {
                covered.push_back(viewport->GetId());
            }
        }

        const vector<Renderer::Viewport*>& registered = m_Services.Compositor.GetViewports();
        for (Renderer::Viewport* viewport : registered)
        {
            const Renderer::ViewportId id = viewport->GetId();
            const bool cover = std::ranges::find(covered, id) != covered.end();
            const auto disabled = std::ranges::find(m_Disabled, id);
            if (cover && disabled == m_Disabled.end() && viewport->IsEnabled())
            {
                viewport->SetEnabled(false);
                m_Disabled.push_back(id);
            }
            else if (!cover && disabled != m_Disabled.end())
            {
                viewport->SetEnabled(true);
                m_Disabled.erase(disabled);
            }
        }
        // A disabled viewport its owner dropped has nothing left to re-enable.
        std::erase_if(m_Disabled,
                      [&registered](const Renderer::ViewportId id)
                      {
                          return std::ranges::none_of(registered,
                                                      [id](const Renderer::Viewport* viewport)
                                                      { return viewport->GetId() == id; });
                      });
    }

    Scene* OverlayWorlds::ResolveOpenerScene(const OverlayWorld& overlay) const
    {
        World* const world = m_Services.Runner.ResolveWorld(overlay.OpenerWorld);
        if (world == nullptr || world->LiveScene != overlay.OpenerScene)
        {
            return nullptr;
        }
        return world->LiveScene;
    }
}
