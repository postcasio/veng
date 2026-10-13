#pragma once

#include <Veng/Veng.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    class PhysicsWorld;

    /// @brief Returns the scene's PhysicsWorld, creating and installing one when it has none.
    ///
    /// A new world is built from the scene's PhysicsSettings (`Scene::TryGetFirst`), or from the
    /// defaults when it carries none, through ToPhysicsWorldInfo, with its debug draw set from
    /// PhysicsSettings::DebugDraw. A world already installed — by the application before start, by
    /// a system started earlier, by anything else — is returned as it is and the settings are not
    /// read, so a consumer whose world needs an input the component cannot express keeps
    /// installing its own.
    /// @param scene  The scene whose world is returned.
    /// @return The scene's world, owned by the scene.
    VE_API PhysicsWorld& EnsurePhysicsWorld(Scene& scene);

    /// @brief Resolves a scene's GravitySource components into world-space evaluator instances.
    ///
    /// Each source is authored in its entity's local frame; the entity's Transform is read as a
    /// world pose (a physics entity is a scene-graph root, so a parent's transform is not composed
    /// in) and used to place the field's direction, origin and region in world space. The result is
    /// what both the physics step's per-body gravity and a character's up-vector query pass to
    /// EvaluateGravity, so a dynamic body and a character in the same place agree by construction.
    /// A non-uniform scale on the source entity is not applied to the region.
    /// @param scene  The scene whose GravitySource components are read.
    /// @param out    Destination vector, cleared then filled.
    VE_API void GatherGravitySources(const Scene& scene, vector<GravitySourceInstance>& out);

    /// @brief Reconciles a scene's bodies against its components and advances the solver one step.
    ///
    /// The whole step, as a free function so a headless tool or a test can drive a scene's physics
    /// without building a SceneSimulation. It runs five passes over the scene's PhysicsWorld:
    /// reconcile (create, re-create and destroy bodies so the world matches the RigidBody/Collider
    /// components), push (place static bodies and drive kinematic ones toward their target pose),
    /// step, pull (write each simulated body's result back into PhysicsPose, and into Transform for
    /// a body whose RigidBody sets SyncTransform), and publish (each Sensor's SensorOverlaps, and
    /// each ContactReport entity's BodyContacts). It then saves every Predicted body's state into
    /// the world's per-tick record under the scene's change tick — the tick the Sim phase stamps —
    /// which BeginPhysicsReplay restores from.
    ///
    /// A body's pose is world space. An entity carrying a RigidBody is therefore expected to be a
    /// scene-graph root: its Transform is read and written as a world pose, and a parent's
    /// transform is not composed into it.
    ///
    /// A no-op when the scene owns no PhysicsWorld; a caller stepping a scene PhysicsSystem has not
    /// started calls EnsurePhysicsWorld first.
    /// @param scene  The scene whose bodies are reconciled and stepped.
    /// @param delta  The step length in seconds.
    VE_API void StepPhysics(Scene& scene, f32 delta);

    /// @brief Opens a rollback replay: predicted bodies go back to tick @p tick, the rest are held.
    ///
    /// The reconciliation calls it after writing the authoritative records onto the predicted set
    /// and before the first replayed tick. It:
    /// - restores every Predicted body to its state at the end of @p tick from the world's per-tick
    ///   record (PhysicsWorld::HasRecordedState), together with its PhysicsPose; a Predicted body
    ///   with no record for @p tick is left as it is;
    /// - captures every other non-static body — its solver state (sleep included), its PhysicsPose,
    ///   the Transform the readback writes, and a driven body's push record — to be put back by
    ///   EndPhysicsReplay;
    /// - marks the world as replaying (PhysicsWorld::IsReplaying), so PhysicsSystem steps each
    ///   replayed tick.
    ///
    /// The restore puts a body where this peer predicted it was at @p tick. A body whose
    /// authoritative state arrives in replicated components is seated on them by its consumer on the
    /// first replayed tick (SystemContext::IsReplayStart), which overrides the pose and velocity and
    /// keeps the rest. The predicted and held sets are fixed here: a body marked or unmarked
    /// Predicted during the replay keeps the role it had when the replay began.
    ///
    /// Inert on a scene with no PhysicsWorld, and on one carrying no Predicted entity, which takes
    /// no part in rollback.
    /// @param scene  The scene whose world replays.
    /// @param tick   The authoritative tick the replay starts from (the reconcile's C).
    /// @pre No replay is open on the scene's world.
    VE_API void BeginPhysicsReplay(Scene& scene, u64 tick);

    /// @brief Closes the replay BeginPhysicsReplay opened, putting every held body back exactly.
    ///
    /// Each body captured at the start is restored to its captured solver state, PhysicsPose,
    /// Transform and push record, so the replay advanced it by no step at all; a held body the replay
    /// destroyed is skipped, and one it re-created is left as created, as is a body it created. The
    /// world's step count goes back to its value before the replay, so GetStepCount keeps counting
    /// live steps, and the replaying mark clears. The predicted bodies keep what the replay made of
    /// them. Inert when no replay is open.
    /// @param scene  The scene whose world ends its replay.
    VE_API void EndPhysicsReplay(Scene& scene);

    /// @brief Builtin Sim system stepping the scene's PhysicsWorld once per tick.
    ///
    /// Registration makes the system resolvable, not ordered: a level that wants physics **names
    /// this system in its own `systems` array**, placed after the systems that produce motion so a
    /// kinematic body's target pose for the tick is already written. A level that does not name it
    /// runs no solver, which is what keeps a physics-free scene free. The system that steps the
    /// world is the one that creates it: OnStart calls EnsurePhysicsWorld, so the scene's
    /// PhysicsSettings (or the defaults) become its world unless one is already installed.
    ///
    /// @warning During a client reconciliation replay (SystemContext::IsReplay), OnUpdate steps only
    /// when the scene carries a Predicted entity or its world is replaying. A scene with none does
    /// not participate in rollback and returns early, so a mispredict of N ticks advances its world
    /// by exactly one step. A scene predicting a body does roll back: the step runs during the
    /// replay, between BeginPhysicsReplay and EndPhysicsReplay, re-simulating the predicted bodies
    /// against kinematic bodies re-driven to each replayed tick, and the bodies nobody predicts are
    /// put back afterwards. See Physics/CLAUDE.md, "The replay gate".
    class VE_API PhysicsSystem final : public SceneSystem
    {
    public:
        /// @brief Gives the scene its PhysicsWorld through EnsurePhysicsWorld.
        /// @param scene    The scene whose world is created when it has none.
        /// @param context  Per-tick services (unused).
        void OnStart(Scene& scene, const SystemContext& context) override;

        /// @brief Reconciles and steps the scene's PhysicsWorld, then draws it when enabled.
        /// @param scene    The scene whose bodies are simulated.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services; IsReplay gates the step, Tick keys the predicted
        ///                 bodies' record, and Debug receives the draw.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Destroys every body the scene's world holds, leaving the world itself alive.
        /// @param scene    The scene whose bodies are released.
        /// @param context  Per-tick services (unused).
        void OnStop(Scene& scene, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::PhysicsSystem, 0xF9E9591959ECD33AULL, "Physics");
