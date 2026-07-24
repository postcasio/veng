#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng::Renderer
{
    class DebugDraw;
}

namespace Veng
{
    /// @brief Construction descriptor for a PhysicsWorld.
    ///
    /// It carries no step rate: a world steps inside the Sim phase at that phase's fixed
    /// SimTickRate, and a second independent rate would need an accumulator and a substep policy
    /// while giving up the determinism, replay and transform interpolation the Sim phase already
    /// supplies.
    struct PhysicsWorldInfo
    {
        /// @brief Gravitational acceleration in metres per second squared.
        vec3 Gravity = vec3(0.0f, -9.81f, 0.0f);
        /// @brief Which layer pairs may collide; must be symmetric.
        CollisionMatrix Matrix = DefaultCollisionMatrix();
        /// @brief Hard ceiling on simultaneously live bodies; exceeding it is a fatal assert.
        ///
        /// It also sizes the world's per-step scratch, which the solver allocates up front and
        /// scales by the body budget — so raising it costs resident memory whether or not the
        /// bodies exist.
        u32 MaxBodies = 4096;
        /// @brief Hard ceiling on broad-phase body pairs tracked in one step.
        u32 MaxBodyPairs = 16384;
        /// @brief Hard ceiling on contact constraints solved in one step.
        u32 MaxContactConstraints = 8192;
    };

    /// @brief A rigid-body simulation space, owned by at most one Scene.
    ///
    /// A Scene optionally owns a PhysicsWorld the way it optionally owns a SceneSimulation, and it
    /// owns none by default — a scene with no physics instantiates nothing and costs nothing.
    /// Bodies are keyed by the Entity they belong to; the components are the authority and the
    /// bodies are their shadow, so the world is reconciled against the scene once per step rather
    /// than mutated by hand.
    ///
    /// **The world's frame is a property of the world, not of any viewer.** Every pose that
    /// crosses this API — PhysicsPose, a body's velocity, a debug-draw vertex — is expressed in
    /// the one frame the world was created in, and that frame does not move because a camera,
    /// a seat, or a presenting peer moved. Anchoring a simulation at a per-viewer origin makes
    /// two peers integrate the same world in different frames, and the divergence is silent.
    ///
    /// The solver behind it is contained by the Native idiom: no public header names a
    /// third-party type, and the library links PRIVATE.
    class VE_API PhysicsWorld
    {
    public:
        /// @brief Backend state, defined in the implementation TU.
        struct Native;

        /// @brief Creates a world from its descriptor.
        /// @param info  Gravity, the collision matrix, and the body budget.
        /// @pre info.Matrix is symmetric (see IsSymmetric).
        /// @return The owned world.
        [[nodiscard]] static Unique<PhysicsWorld> Create(const PhysicsWorldInfo& info);

        /// @brief Destroys every live body and releases the backend state.
        ~PhysicsWorld();

        /// @brief Worlds are single-owner; copying one would alias its bodies.
        PhysicsWorld(const PhysicsWorld&) = delete;
        /// @brief Worlds are single-owner; copying one would alias its bodies.
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        /// @brief Returns the backend state, for implementation code inside the engine.
        [[nodiscard]] Native& GetNative() const { return *m_Native; }

        /// @brief Returns the descriptor this world was created from.
        [[nodiscard]] const PhysicsWorldInfo& GetInfo() const { return m_Info; }

        /// @brief Sets the gravitational acceleration applied to every dynamic body.
        /// @param gravity  Acceleration in metres per second squared.
        void SetGravity(vec3 gravity);

        /// @brief Returns the gravitational acceleration applied to every dynamic body.
        [[nodiscard]] vec3 GetGravity() const;

        /// @brief Brings @p entity's body into line with its components, creating it if absent.
        ///
        /// Idempotent, which is what lets the reconcile pass call it for every physics entity every
        /// tick: a body that already exists with settings equal to @p body and @p collider is left
        /// exactly as it is, pose and velocity included. Any difference re-creates the body from
        /// @p pose, discarding its velocity and contact state — the components are the authority
        /// and the body is their shadow.
        /// @param entity    The entity the body belongs to.
        /// @param body      The motion, layer, mass and damping settings.
        /// @param collider  The primitive shape and its surface properties.
        /// @param pose      The world-space pose to place a newly created body at.
        void CreateBody(Entity entity, const RigidBody& body, const Collider& collider,
                        const PhysicsPose& pose);

        /// @brief Destroys @p entity's body; a no-op when it has none.
        /// @param entity  The entity whose body to destroy.
        void DestroyBody(Entity entity);

        /// @brief Destroys every live body, leaving the world itself usable.
        void DestroyAllBodies();

        /// @brief Whether @p entity currently has a body in this world.
        [[nodiscard]] bool HasBody(Entity entity) const;

        /// @brief Returns the number of live bodies.
        [[nodiscard]] u32 GetBodyCount() const;

        /// @brief Fills @p out with every entity that currently has a body, in ascending slot order.
        ///
        /// The reconcile pass's orphan sweep reads it: a body whose entity has lost its components
        /// (or died) is only findable from this side. Sorted, so the sweep is order-stable.
        /// @param out  Destination vector, cleared then filled.
        void GetBodyEntities(vector<Entity>& out) const;

        /// @brief Teleports @p entity's body to @p pose, clearing nothing else.
        ///
        /// A teleport, not a move: it does not sweep, so a body placed inside geometry is resolved
        /// by the next step's penetration recovery. MoveKinematicBody is the swept alternative.
        /// @param entity  The entity whose body to place; a no-op when it has none.
        /// @param pose    The world-space pose to place it at.
        void SetBodyPose(Entity entity, const PhysicsPose& pose);

        /// @brief Drives a kinematic body toward @p target over one step, so it sweeps.
        ///
        /// Sets the velocity that reaches @p target in @p delta seconds, which is what lets a
        /// kinematic mover push dynamic bodies instead of passing through them.
        /// @param entity  The entity whose body to drive; a no-op when it has none.
        /// @param target  The world-space pose to reach at the end of the step.
        /// @param delta   The step length in seconds; must be positive.
        void MoveKinematicBody(Entity entity, const PhysicsPose& target, f32 delta);

        /// @brief Returns @p entity's body pose, or nullopt when it has no body.
        [[nodiscard]] optional<PhysicsPose> GetBodyPose(Entity entity) const;

        /// @brief Sets a body's linear velocity in metres per second; a no-op when it has none.
        /// @param entity    The entity whose body to drive.
        /// @param velocity  World-space linear velocity.
        void SetLinearVelocity(Entity entity, vec3 velocity);

        /// @brief Returns a body's linear velocity in metres per second, or zero when it has none.
        [[nodiscard]] vec3 GetLinearVelocity(Entity entity) const;

        /// @brief Sets a body's angular velocity in radians per second; a no-op when it has none.
        /// @param entity    The entity whose body to drive.
        /// @param velocity  World-space angular velocity as an axis-angle vector.
        void SetAngularVelocity(Entity entity, vec3 velocity);

        /// @brief Returns a body's angular velocity in radians per second, or zero when it has none.
        [[nodiscard]] vec3 GetAngularVelocity(Entity entity) const;

        /// @brief Advances the simulation by one fixed step.
        ///
        /// Called once per Sim tick with that phase's fixed delta. A variable delta is legal but
        /// forfeits repeatability, which is the property the whole module is arranged around.
        /// @param delta  The step length in seconds.
        void Step(f32 delta);

        /// @brief Returns how many times Step has run, counting from world creation.
        ///
        /// The physics clock. It must advance in lockstep with the sim tick; the replay gate in
        /// PhysicsSystem exists to keep it there.
        [[nodiscard]] u64 GetStepCount() const { return m_StepCount; }

        /// @brief Captures the whole simulation state — poses, velocities, contacts, sleep — as bytes.
        ///
        /// The save side of the rollback seam. The bytes are opaque and version-locked to the
        /// solver build that produced them; nothing outside this class interprets them.
        /// @return The captured state.
        [[nodiscard]] vector<u8> SaveState() const;

        /// @brief Restores a state previously captured by SaveState.
        ///
        /// Restoring rewinds the solver's internals but not the step count, which keeps counting
        /// the steps this world actually ran.
        /// @param state  Bytes from SaveState on a world with the same body set.
        /// @return Success, or an error when the bytes are not a readable state.
        VoidResult RestoreState(std::span<const u8> state);

        /// @brief Returns an order-independent hash of every body's pose.
        ///
        /// Hashes the raw bit patterns of the position and orientation components, so two worlds
        /// hash equal only when their bodies are bit-identically placed. This is the observable a
        /// determinism check compares, including the cross-platform one the solver is built for.
        [[nodiscard]] u64 HashPoses() const;

        /// @brief Enables or disables the per-step debug visualization.
        /// @param enabled  Whether PhysicsSystem draws this world's bodies each tick.
        void SetDebugDrawEnabled(const bool enabled) { m_DebugDrawEnabled = enabled; }

        /// @brief Whether the per-step debug visualization is enabled.
        [[nodiscard]] bool IsDebugDrawEnabled() const { return m_DebugDrawEnabled; }

        /// @brief Draws every body's shape, state and active contacts into @p sink.
        ///
        /// Shapes are drawn as wireframes tinted by motion type and dimmed while the body sleeps;
        /// each active contact is a short normal spike at the contact point. Immediate mode — the
        /// sink clears each frame, so this is called every frame the visualization should appear.
        /// @param sink  The accumulator the primitives are pushed into.
        void DrawDebug(Renderer::DebugDraw& sink) const;

    private:
        /// @brief Constructs the world; use Create.
        /// @param info  The descriptor to construct from.
        explicit PhysicsWorld(const PhysicsWorldInfo& info);

        /// @brief The descriptor this world was created from.
        PhysicsWorldInfo m_Info;
        /// @brief The backend state.
        Unique<Native> m_Native;
        /// @brief How many times Step has run.
        u64 m_StepCount = 0;
        /// @brief Whether PhysicsSystem draws this world's bodies each tick.
        bool m_DebugDrawEnabled = false;
    };
}
