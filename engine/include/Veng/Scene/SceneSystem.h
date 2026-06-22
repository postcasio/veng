#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class Input;

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
    };

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
