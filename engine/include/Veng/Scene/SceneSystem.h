#pragma once

#include <Veng/Veng.h>
#include <Veng/InputRouter.h>
#include <Veng/Math/Ray.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>
#include <Veng/WorldInstanceId.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class Input;
    class TaskSystem;
}

namespace Veng::Audio
{
    class AudioEngine;
}

namespace Veng::Renderer
{
    class DebugDraw;
}

namespace Veng
{

    /// @brief Stable identity of a registered SceneSystem, authored exactly like a TypeId/AssetId.
    ///
    /// A SceneSystem subclass declares one through VE_SYSTEM; SystemRegistry keys its
    /// catalog on it and a Level names the active ordered set by it. It is a new id
    /// space alongside AssetId/TypeId, minted with `vengc generate-id`.
    using SystemId = u64;

    /// @brief The empty SystemId, distinct from every authored id.
    inline constexpr SystemId InvalidSystemId = 0;

    /// @brief Identity trait every registered SceneSystem subclass specialises.
    ///
    /// Unspecialised by default — a SceneSystem subclass declares its identity by
    /// specialising this trait through the VE_SYSTEM macro, which emits a stable
    /// SystemId and a display name. SystemRegistry::Register reads the trait, so a
    /// system registered without a VE_SYSTEM fails to compile. SystemIdOf and
    /// SystemNameOf read the members directly.
    /// @tparam T The concrete SceneSystem subclass.
    template <class T>
    struct VengSystem;

    /// @brief The stable SystemId of a registered system, read as a compile-time constant off its trait.
    ///
    /// Independent of registration order and of any SystemRegistry instance.
    /// @tparam T The concrete SceneSystem subclass.
    /// @return The authored SystemId.
    template <class T>
    constexpr SystemId SystemIdOf()
    {
        return VengSystem<T>::Id;
    }

    /// @brief The display name of a registered system, read off its trait.
    /// @tparam T The concrete SceneSystem subclass.
    /// @return The authored display name.
    template <class T>
    string SystemNameOf()
    {
        return VengSystem<T>::Name();
    }

    /// @brief The resolved view a simulation's primary presenting viewport supplies to its systems.
    ///
    /// A scene-scoped view descriptor — the camera, region, and UI scale of the sim's primary
    /// presenting viewport — delivered without a Renderer::Viewport so Sim systems stay
    /// viewport-agnostic. The pure pick/project free functions below (ScreenToWorldRay,
    /// WorldToRegion, WorldToDocument, DocumentExtent) operate over it, so a system picks and
    /// projects from its SystemContext's View + Pointer without holding a viewport.
    ///
    /// Two constraints follow from how the engine assembles it (see SystemContext::View):
    /// - It is the view **as of the last completed frame** — view pushes run after ticks — so a
    ///   system that itself drives the camera this tick reads the same-tick camera from the scene
    ///   (or builds its own CameraView) and combines it with View.Region.
    /// - It is the **primary presenter's** view, so unprojecting Pointer through it is valid only
    ///   single-presenter; with several viewports presenting one scene the pointer's owning region
    ///   may differ.
    struct SystemViewInfo
    {
        /// @brief The resolved camera the presenting viewport rendered the scene through last frame.
        CameraView Camera;
        /// @brief The presenting viewport's placement region, in framebuffer pixels.
        Renderer::ViewportRegion Region;
        /// @brief The scale the viewport's documents lay out at (region pixels per logical point).
        f32 UiScale = 1.0f;
    };

    /// @brief Who holds simulation authority for the peer running this tick.
    ///
    /// A standalone or server peer is Server — its Sim phase is authoritative over
    /// Authority::Server state. A Client peer displays server-owned state and simulates
    /// only its client-local (view) entities. Carried on SystemContext so an
    /// authority-gated Sim system can query the peer's role; a standalone app is always
    /// Server.
    enum class NetRole
    {
        /// @brief This peer owns and advances server-authoritative simulation state.
        Server,
        /// @brief This peer displays replicated server state and simulates only client-local entities.
        Client,
    };

    /// @brief Per-tick services handed to every SceneSystem.
    ///
    /// Borrowed for the duration of the call: a system reads from these but does
    /// not own them. The Input reference is the always-present frame-coherent input
    /// service; in headless mode it reports the neutral all-zeros state rather than
    /// being absent, so an input-reading system needs no null-guard.
    struct SystemContext
    {
        /// @brief The asset manager a system loads or builds resources through.
        AssetManager& Assets;
        /// @brief The always-present frame-coherent input service; present-but-neutral (all zeros) in headless mode.
        const Input& Input;
        /// @brief The task system a system runs async work through (streaming gathers, off-thread builds).
        ///
        /// A scene-agnostic Application service, always present. A reference (not a pointer), so an
        /// async-using system needs no null-guard.
        TaskSystem& Tasks;
        /// @brief The device-wide audio engine a system triggers sound through.
        ///
        /// The mixer-facing engine every system reaches to fire one-shots (PlayOneShot / PlayAt) and
        /// set the background music (Music()). A scene-agnostic Application service backed by a null
        /// device when there is no hardware, so every call is a no-op that still tracks the request
        /// and no audio-triggering system needs a null-guard.
        Audio::AudioEngine& Audio;
        /// @brief This frame's free-pointer owner + region-local position; default-empty when unrouted.
        ///
        /// The InputMappingSystem reads it to build each seat's region-gated pointer view. Its
        /// default (Owner == Entity::Null) leaves every seat reading neutral mouse, so a headless or
        /// pointer-free tick needs no guard. The engine scopes it per scene: a routing reaches only
        /// the sim whose scene the pointer's owning viewport presents.
        PointerRouting Pointer;
        /// @brief The sim's primary presenting viewport's resolved view, or nullopt when unpresented.
        ///
        /// Populated by the engine from the sole/primary registered Presented viewport whose retained
        /// scene is this sim's — so it is the view **as of the last completed frame** (view pushes run
        /// after ticks) and the **primary presenter's** view (see SystemViewInfo). nullopt for a
        /// view-less sim (a background/offscreen scene no viewport presents) and on a viewport's very
        /// first tick before any view has been pushed.
        optional<SystemViewInfo> View;
        /// @brief The primary presenting viewport's immediate-mode debug-draw sink, or null when unpresented.
        ///
        /// The sibling of View, resolved from the same viewport's accumulator so gameplay systems push
        /// debug lines/billboards into the scene's own view. Null for a view-less sim and headless (a
        /// value SystemViewInfo cannot carry a mutable sink, so it rides SystemContext directly).
        Renderer::DebugDraw* Debug = nullptr;
        /// @brief The fixed simulation tick number.
        ///
        /// In the Sim phase this is the tick being advanced (monotonic, +1 per fixed step); in the
        /// View phase it is the last completed Sim tick. Zero before the first tick runs. The unit of
        /// time a replicated simulation keys input and snapshots by.
        u64 Tick = 0;
        /// @brief Interpolation fraction into the next Sim tick, in [0, 1); View phase only.
        ///
        /// The frame's residual accumulator over the fixed step: a View system (a camera rig) blends
        /// toward the coming tick by this, matching the render gather's transform interpolation so the
        /// camera and the meshes it frames agree. Zero in the Sim phase (a fixed step has no residual).
        f32 Alpha = 0.0f;
        /// @brief Which peer's authority this tick runs under; Server for a standalone app.
        ///
        /// An authority-gated Sim system reads it to act only where this peer owns the state. Inert
        /// until a client/server split exists — every standalone tick is Server.
        NetRole Role = NetRole::Server;

        /// @brief The world instance this tick runs in — the runner handle of the ticking world.
        ///
        /// The WorldRunner mints one id per world for its whole open lifetime and never reuses it
        /// (see WorldInstanceId), so a system that keeps host-side state spanning more than one world
        /// can tell which world a tick belongs to — tagging a record with the world that produced it,
        /// or telling its own live entity in this scene from an identically-indexed one in another
        /// world's scene (entity handles are per-scene and alias across scenes). Default-invalid
        /// (names no world) for a context a caller assembles without a runner.
        WorldInstanceId World;

        /// @brief Whether the presenting seat currently holds gameplay focus this frame.
        ///
        /// Stamped from the InputRouter's focus state (false headless — no window owns focus). The
        /// InputMappingSystem reads it to exclude a focus-gated InputMappingContext (one authored
        /// `requiresGameplayFocus`) from a seat's effective active list while the seat is not
        /// gameplay-focused — a pure evaluation, never a mutation of the authored InputContextStack.
        /// A HUD/menu owning the cursor thus silences gameplay bindings without any stack surgery.
        /// The transition itself — capturing or releasing this focus — is driven by a system through
        /// the builtin FocusRequest component (Veng/Scene/Requests.h), the engine owning the token.
        bool GameplayFocused = false;

        /// @brief Whether this is the first Sim step of the frame's fixed-step sequence.
        ///
        /// A frame runs 0..N Sim steps under the fixed-timestep accumulator; this is true only on
        /// step 0, so a system accumulating across the frame's steps (InputMappingSystem folding
        /// per-tick action edges into a frame-accumulated view) resets its accumulation here. False
        /// in the View phase, which is not a Sim step.
        bool FirstStepThisFrame = false;

        /// @brief Whether this Sim step is a client reconciliation replay, not a live tick.
        ///
        /// A client that mispredicts restores its predicted set to the authoritative state and
        /// replays its recorded inputs forward through the real Sim systems (see the networking
        /// guide). Those replayed ticks re-run control and movement to re-derive state — but a system
        /// with an *external* side effect (spawning an entity, triggering a sound, emitting an event
        /// outward) must NOT repeat it per replayed tick, since the effect already fired on the
        /// entity's first live simulation of that tick. Such a system gates the side effect on this
        /// flag; a pure state advancer ignores it. False on every live tick and in the View phase.
        bool IsReplay = false;

        /// @brief Returns a copy of this context with Alpha set to @p alpha.
        ///
        /// The View-phase context is the Sim-phase context plus the frame's interpolation fraction;
        /// this produces it without restating the borrowed services. The reference members rebind by
        /// copy-construction (SystemContext is not copy-assignable — its members are references).
        /// @param alpha  The interpolation fraction to carry.
        /// @return A copy with Alpha = @p alpha.
        [[nodiscard]] SystemContext WithAlpha(const f32 alpha) const
        {
            SystemContext copy = *this;
            copy.Alpha = alpha;
            return copy;
        }
    };

    /// @brief Unprojects a region-local point into a world-space ray through a resolved view.
    ///
    /// The free-function form of Viewport::ScreenToWorldRay over a SystemViewInfo: maps @p regionPoint
    /// (region-local framebuffer pixels, e.g. PointerRouting::LocalPosition) to NDC across the region
    /// and unprojects it through the view's camera. So a Sim system picks from its SystemContext's
    /// View + Pointer without holding a Renderer::Viewport.
    /// @param view         The resolved camera + region + UI scale.
    /// @param regionPoint  A point in region-local framebuffer pixels ([0, Region.Extent]).
    /// @return The world-space ray through @p regionPoint, or nullopt when the region has a zero extent.
    [[nodiscard]] inline optional<Ray> ScreenToWorldRay(const SystemViewInfo& view,
                                                        const vec2 regionPoint)
    {
        if (view.Region.Extent.x == 0 || view.Region.Extent.y == 0)
        {
            return std::nullopt;
        }

        // [0,1] (top-left origin) to NDC. The engine projection bakes the Vulkan Y flip, so a
        // top-left fraction maps to NDC directly without a second flip.
        const vec2 fraction = regionPoint / vec2(view.Region.Extent);
        const vec2 ndc = fraction * 2.0f - 1.0f;

        const mat4 invViewProj = glm::inverse(view.Camera.ViewProjection());
        const vec4 nearClip = invViewProj * vec4(ndc, 0.0f, 1.0f);
        const vec4 farClip = invViewProj * vec4(ndc, 1.0f, 1.0f);
        const vec3 nearWorld = vec3(nearClip) / nearClip.w;
        const vec3 farWorld = vec3(farClip) / farClip.w;

        // The origin is the unprojected near-plane point, not the camera position: under an
        // orthographic projection every pixel's ray is parallel and the camera position lies on
        // none of them; under perspective the near point sits on the eye ray, so both agree.
        return Ray{
            .Origin = nearWorld,
            .Direction = glm::normalize(farWorld - nearWorld),
        };
    }

    /// @brief Projects a world point into region-local pixels through a resolved view.
    ///
    /// The free-function form of Viewport::WorldToRegion: composes ProjectToScreen with the view's
    /// camera and region extent, so (0,0) is the region's top-left.
    /// @param view   The resolved camera + region + UI scale.
    /// @param world  The world-space point to project.
    /// @return The region-local pixel position, or nullopt when the point is behind the camera.
    [[nodiscard]] inline optional<vec2> WorldToRegion(const SystemViewInfo& view, const vec3 world)
    {
        return ProjectToScreen(view.Camera, world, vec2(view.Region.Extent));
    }

    /// @brief Projects a world point into document logical points through a resolved view.
    ///
    /// The free-function form of Viewport::WorldToDocument: WorldToRegion divided by the view's UI
    /// scale, the logical-point space a hosted Gui::Document lays out and hit-tests in. Performs no
    /// in-region rejection — a point outside [0, DocumentExtent] still returns a value.
    /// @param view   The resolved camera + region + UI scale.
    /// @param world  The world-space point to project.
    /// @return The document-space position, or nullopt when the point is behind the camera.
    [[nodiscard]] inline optional<vec2> WorldToDocument(const SystemViewInfo& view,
                                                        const vec3 world)
    {
        const optional<vec2> region = WorldToRegion(view, world);
        if (!region.has_value())
        {
            return std::nullopt;
        }
        return *region / view.UiScale;
    }

    /// @brief Returns a resolved view's region extent in document logical points.
    ///
    /// The free-function form of Viewport::GetDocumentExtent: Region.Extent divided by UiScale — the
    /// bounds a WorldToDocument result is checked against (a value at or beyond it lies off-region).
    /// @param view  The resolved camera + region + UI scale.
    /// @return The region extent in document logical points.
    [[nodiscard]] inline vec2 DocumentExtent(const SystemViewInfo& view)
    {
        return vec2(view.Region.Extent) / view.UiScale;
    }

    /// @brief Whether this peer simulates @p entity under the running tick's authority.
    ///
    /// The authority filter the builtin Sim systems that advance Authority::Server state
    /// (MovementSystem, the motion systems) consult before touching an entity: a Server-tier entity is
    /// simulated only by a Server-role peer, a Local-tier entity is always simulated locally (a
    /// client-local view/UI entity), a Remote-tier entity is never simulated (it is the client-side
    /// mirror the interpolation system displays), and a Predicted-tier entity is always simulated
    /// locally (the client-side prediction stance over an entity its own seat controls, re-run each
    /// client tick ahead of the server). An entity with no Authority component defaults to Server-tier.
    /// On a standalone or server peer (Role::Server) every Server-tier entity passes, so single-player
    /// behaviour is unchanged; on a client the peer skips Server/Remote-tier entities so its Sim phase
    /// never fights the snapshot stream, runs its Predicted set, and still lets an AI or
    /// server-authoritative Intent producer advance the state it owns.
    /// @param context  The per-tick services carrying this peer's NetRole.
    /// @param scene    The scene @p entity lives in.
    /// @param entity   The entity whose simulation authority is queried (must be alive).
    /// @return True when this peer advances @p entity's simulation this tick.
    [[nodiscard]] VE_API bool HasAuthority(const SystemContext& context, const Scene& scene,
                                           Entity entity);

    /// @brief A unit of gameplay logic over a Scene, registered via the module host
    /// and ticked by a SceneSimulation.
    ///
    /// Registered into the host-owned SystemRegistry exactly like a reflected type;
    /// the SceneSimulation driver instantiates and ticks the registered systems.
    /// The runtime app and the editor's Play mode tick the same systems, in
    /// registration order.
    class SceneSystem
    {
    public:
        /// @brief The tick pass a system runs in: deterministic simulation, or client-local view derivation.
        ///
        /// Sim systems advance replicable game state (control, movement, rules); View
        /// systems derive purely local presentation from finalized Sim state (a camera
        /// rig, blends, shake) and are never authoritative or on the wire. A
        /// SceneSimulation runs all Sim systems before all View systems each tick, so a
        /// View system reads the state the Sim phase finalized this tick.
        enum class Phase
        {
            /// @brief Deterministic, replicable simulation; runs first each tick.
            Sim,
            /// @brief Client-local view derivation; runs after every Sim system each tick.
            View,
        };

        /// @brief Virtual destructor; systems are owned through SceneSystem pointers.
        virtual ~SceneSystem() = default;

        /// @brief Returns the tick pass this system runs in.
        ///
        /// Defaults to Phase::Sim, so a system is part of the deterministic simulation
        /// unless it overrides this to Phase::View.
        /// @return The system's phase.
        [[nodiscard]] virtual Phase GetPhase() const { return Phase::Sim; }

        /// @brief Called once when play/simulation begins, before the first OnUpdate.
        ///
        /// The default does nothing.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (assets, input).
        virtual void OnStart(Scene& scene, const SystemContext& context) {}

        /// @brief Called once per frame to advance the system's logic.
        /// @param scene    The scene the system operates over.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (assets, input).
        virtual void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) = 0;

        /// @brief Called once when play/simulation ends, after the last OnUpdate.
        ///
        /// The default does nothing.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (assets, input).
        virtual void OnStop(Scene& scene, const SystemContext& context) {}
    };
}

/// @brief Declares a SceneSystem subclass's catalog identity by specialising VengSystem\<T\>.
///
/// Emits a stable SystemId and a display name, so SystemRegistry::Register stores
/// `{ SystemId, Name, factory }` and the catalog enumerates and resolves the system
/// without instantiating it. Authored exactly like a TypeId: the id is a hardcoded
/// 0x…ULL literal for engine systems or a `vengc generate-id` value for game systems,
/// and two systems claiming one id is a fatal collision assert at registration.
/// @param Type        The concrete SceneSystem subclass.
/// @param IdLiteral   The authored SystemId (uppercase hex 0x…ULL).
/// @param NameLiteral The display name string literal.
#define VE_SYSTEM(Type, IdLiteral, NameLiteral)                                                    \
    template <>                                                                                    \
    struct ::Veng::VengSystem<Type>                                                                \
    {                                                                                              \
        static constexpr ::Veng::SystemId Id = (IdLiteral);                                        \
        static ::Veng::string Name() { return (NameLiteral); }                                     \
    }
