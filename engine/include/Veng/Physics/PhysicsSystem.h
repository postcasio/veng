#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Reconciles a scene's bodies against its components and advances the solver one step.
    ///
    /// The whole step, as a free function so a headless tool or a test can drive a scene's physics
    /// without building a SceneSimulation. It runs four passes over the scene's PhysicsWorld:
    /// reconcile (create, re-create and destroy bodies so the world matches the RigidBody/Collider
    /// components), push (place static bodies and drive kinematic ones toward their target pose),
    /// step, and pull (write each simulated body's result back into PhysicsPose, and into
    /// Transform for a body whose RigidBody sets SyncTransform).
    ///
    /// A body's pose is world space. An entity carrying a RigidBody is therefore expected to be a
    /// scene-graph root: its Transform is read and written as a world pose, and a parent's
    /// transform is not composed into it.
    ///
    /// A no-op when the scene owns no PhysicsWorld.
    /// @param scene  The scene whose bodies are reconciled and stepped.
    /// @param delta  The step length in seconds.
    VE_API void StepPhysics(Scene& scene, f32 delta);

    /// @brief Builtin Sim system stepping the scene's PhysicsWorld once per tick.
    ///
    /// Registration makes the system resolvable, not ordered: a level that wants physics **names
    /// this system in its own `systems` array**, placed after the systems that produce motion so a
    /// kinematic body's target pose for the tick is already written. A level that does not name it
    /// runs no solver, which is what keeps a physics-free scene free.
    ///
    /// @warning A scene with a PhysicsWorld does not participate in client reconciliation
    /// rollback. OnUpdate returns early when SystemContext::IsReplay is set, because a replay
    /// re-runs the whole Sim phase while the solver's own state — velocities, the contact cache,
    /// sleep — is not in the prediction history and is therefore never restored. Stepping anyway
    /// would advance the physics clock once per replayed tick against state that was never
    /// rewound, and the drift from the sim tick is permanent. The gate makes a mispredict of N
    /// ticks advance the world by exactly one step.
    class VE_API PhysicsSystem final : public SceneSystem
    {
    public:
        /// @brief Reconciles and steps the scene's PhysicsWorld, then draws it when enabled.
        /// @param scene    The scene whose bodies are simulated.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services; IsReplay gates the step and Debug receives the draw.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Destroys every body the scene's world holds, leaving the world itself alive.
        /// @param scene    The scene whose bodies are released.
        /// @param context  Per-tick services (unused).
        void OnStop(Scene& scene, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::PhysicsSystem, 0xF9E9591959ECD33AULL, "Physics");
