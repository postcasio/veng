#pragma once

#include <Veng/Veng.h>
#include <Veng/Physics/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

#include <span>

// Veng/Net/LagCompensation.h — judging a client's query against the world as that client saw it.
//
// A client draws remote entities in the past (RemoteInterpolationSystem) and stamps the tick it was
// drawing onto each input (TickedInput::ViewDelayTicks, read back server-side through SeatViewTick).
// The server keeps a short history of the poses of the bodies that opt in (LagCompensated), and a
// RewindScope moves those bodies back to a view tick for as long as it lives, so the ordinary pure
// physics queries made inside it — a ray, a sweep, an overlap — hit what that client saw. It favours
// the querying client: a body that has just moved behind cover can still be hit for up to
// MaxRewindSeconds, and that cap is the whole bound on the advantage.

namespace Veng
{
    class PhysicsWorld;
    class Scene;

    /// @brief Marks an entity whose body a server records, so a query can be judged against its past pose.
    ///
    /// Opt-in per entity: PoseHistorySystem records the physics pose of every entity carrying it and a
    /// body, and a RewindScope moves only these. Static structure and unmarked bodies stay where they
    /// are, so marking what a query is meant to hit (a moving target) and nothing else keeps the
    /// rewind cheap. Authorable and fieldless; the server reads it, and it need not replicate.
    struct LagCompensated
    {
    };

    /// @brief One recorded tick of a body's physics state.
    struct PoseSample
    {
        /// @brief The sim tick whose post-step state this is.
        u64 Tick = 0;
        /// @brief The body's world-space pose after that tick's step.
        PhysicsPose Pose;
        /// @brief The body's linear velocity after that tick's step, in metres per second.
        vec3 LinearVelocity{0.0f};
        /// @brief The body's angular velocity after that tick's step, in radians per second.
        vec3 AngularVelocity{0.0f};
    };

    /// @brief A body's pose and velocities at a (fractional) past tick, as a history reconstructs it.
    struct RewoundBodyPose
    {
        /// @brief The world-space pose.
        PhysicsPose Pose;
        /// @brief The linear velocity, in metres per second.
        vec3 LinearVelocity{0.0f};
        /// @brief The angular velocity, in radians per second.
        vec3 AngularVelocity{0.0f};
    };

    /// @brief A bounded, per-entity record of recent body poses, the store a rewind reads.
    ///
    /// Each recorded entity holds a ring of PoseSamples covering at most GetMaxRewindTicks() ticks
    /// behind the newest one; older samples are dropped as new ones arrive, and an entity not
    /// recorded on a tick loses its ring when that tick closes (EndTick), so a destroyed or unmarked
    /// entity leaves nothing behind. Pure and device-free: it records what it is given and never reads
    /// a scene or a physics world itself. A Scene owns one (Scene::SetPoseHistory), filled by
    /// PoseHistorySystem.
    class VE_API PoseHistory
    {
    public:
        /// @brief How far back the history reaches, and the cadence its reconstruction follows.
        struct Settings
        {
            /// @brief The longest a query may be rewound, in seconds; the history holds no more.
            ///
            /// It is also the bound on how far a lag-compensated query favours the querying client:
            /// a body that moved behind cover can still be hit for up to this long after it did.
            f64 MaxRewindSeconds = 0.25;
            /// @brief The server ticks between snapshots — the samples a client's view blended.
            ///
            /// A client interpolates between the snapshot ticks it received, so RewoundPose blends
            /// between the samples at multiples of this and not at every tick, reconstructing the
            /// pose the client drew rather than a finer one. Zero blends every tick.
            u64 SnapshotInterval = 2;

            /// @brief Member-wise equality, so a caller re-applying unchanged settings trims nothing.
            bool operator==(const Settings&) const = default;
        };

        /// @brief Constructs an empty history with the default settings.
        PoseHistory() = default;

        /// @brief Constructs an empty history with the given settings.
        /// @param settings  The reach and cadence.
        explicit PoseHistory(const Settings& settings) : m_Settings(settings) {}

        /// @brief Replaces the settings, trimming every ring to the new reach.
        /// @param settings  The new reach and cadence.
        void SetSettings(const Settings& settings);

        /// @brief Returns the current settings.
        [[nodiscard]] const Settings& GetSettings() const { return m_Settings; }

        /// @brief Sets the sim tick rate the reach in seconds is converted to ticks at, trimming every ring.
        ///
        /// PoseHistorySystem keeps it equal to the rate its world steps at.
        /// @param ticksPerSecond  Sim ticks per second; must be positive.
        void SetSimTickRate(f64 ticksPerSecond);

        /// @brief Returns the sim tick rate the reach is converted at (60 until set).
        [[nodiscard]] f64 GetSimTickRate() const { return m_SimTickRate; }

        /// @brief The reach in whole ticks: MaxRewindSeconds at the sim tick rate, rounded down.
        [[nodiscard]] u64 GetMaxRewindTicks() const;

        /// @brief Appends a sample to @p entity's ring, dropping what falls out of reach.
        ///
        /// A sample for the tick the ring already ends on replaces that one.
        /// @param entity  The recorded entity.
        /// @param sample  Its state at sample.Tick.
        /// @pre sample.Tick is not older than the newest sample already held for @p entity.
        void Record(Entity entity, const PoseSample& sample);

        /// @brief Closes a tick: drops every ring not recorded at @p tick and makes @p tick the newest.
        ///
        /// The recorder calls it once per tick after its Record calls, so an entity that died, lost
        /// its body or lost LagCompensated since the last tick has its ring dropped.
        /// @param tick  The tick just recorded.
        void EndTick(u64 tick);

        /// @brief Drops every ring and forgets the newest tick.
        void Clear();

        /// @brief Returns the newest closed tick, or nullopt before the first EndTick.
        [[nodiscard]] optional<u64> GetNewestTick() const { return m_NewestTick; }

        /// @brief Clamps a view tick into the history's reach: [newest - GetMaxRewindTicks(), newest].
        ///
        /// A view older than the reach rewinds only that far, so a badly lagging client gets less
        /// compensation, never more. Returned unchanged before the first EndTick.
        /// @param viewTick  The (fractional) tick a client viewed.
        /// @return The tick a rewind to @p viewTick actually stands at.
        [[nodiscard]] f64 ClampViewTick(f64 viewTick) const;

        /// @brief Reconstructs @p entity's pose and velocities at a (fractional) tick.
        ///
        /// Blends as SampleRemoteInterpolation blends a client's view — position and velocities
        /// linearly, rotation spherically — between the two samples bracketing @p viewTick, where the
        /// candidates are the samples at multiples of SnapshotInterval (the ticks a snapshot carried)
        /// plus the ring's oldest and newest samples. A tick outside the ring holds at its nearer end.
        /// @param entity    The recorded entity.
        /// @param viewTick  The (fractional) tick to reconstruct.
        /// @return The reconstruction, or nullopt when @p entity has no ring.
        [[nodiscard]] optional<RewoundBodyPose> RewoundPose(Entity entity, f64 viewTick) const;

        /// @brief Returns @p entity's ring, oldest first; empty when it has none.
        /// @param entity  The recorded entity.
        [[nodiscard]] std::span<const PoseSample> GetSamples(Entity entity) const;

        /// @brief Fills @p out with every recorded entity, in ascending slot order.
        /// @param out  Destination vector, cleared then filled.
        void GetEntities(vector<Entity>& out) const;

        /// @brief Returns how many entities hold a ring.
        [[nodiscard]] usize GetEntityCount() const { return m_Rings.size(); }

    private:
        /// @brief Drops the samples of @p ring older than the reach behind its newest one.
        /// @param ring  The ring to trim.
        void Trim(vector<PoseSample>& ring) const;

        /// @brief Trims every ring to the current reach.
        void TrimAll();

        /// @brief The reach and cadence.
        Settings m_Settings;
        /// @brief The sim tick rate the reach in seconds converts at.
        f64 m_SimTickRate = 60.0;
        /// @brief Each recorded entity's samples, oldest first.
        unordered_map<Entity, vector<PoseSample>> m_Rings;
        /// @brief The newest closed tick; nullopt before the first EndTick.
        optional<u64> m_NewestTick;
    };

    /// @brief Returns @p scene's pose history, installing one first when it has none.
    ///
    /// Re-applies @p settings to an installed history only when they differ, so a caller may push its
    /// settings every tick.
    /// @param scene     The scene to own the history.
    /// @param settings  The reach and cadence to install or apply.
    /// @return The scene's history.
    VE_API PoseHistory& EnsurePoseHistory(Scene& scene, const PoseHistory::Settings& settings);

    /// @brief Reconstructs an entity's pose at a past tick from its scene's pose history.
    /// @param scene     The scene whose history to read.
    /// @param entity    The recorded entity.
    /// @param viewTick  The (fractional) tick to reconstruct.
    /// @return The reconstruction, or nullopt when the scene has no history or @p entity no ring.
    [[nodiscard]] VE_API optional<RewoundBodyPose> RewoundPose(const Scene& scene, Entity entity,
                                                               f64 viewTick);

    /// @brief Server-only Sim system recording each LagCompensated body's pose into the scene's history.
    ///
    /// Each Sim tick it records, for every entity carrying LagCompensated and a body, the body's pose
    /// and velocities, then closes the tick (PoseHistory::EndTick). A level that lag-compensates
    /// **names it immediately after PhysicsSystem**, so the recorded state is the tick's post-step
    /// state — the same state that tick's snapshot carries. It installs a default history on a scene
    /// that has none (a host that tunes the reach installs its own first, through
    /// EnsurePoseHistory) and keeps its tick rate equal to the step it runs at. It does nothing on a
    /// client or during a replay, and a scene with no physics world records nothing.
    class VE_API PoseHistorySystem final : public SceneSystem
    {
    public:
        /// @brief Records this tick's LagCompensated body poses.
        /// @param scene    The scene whose bodies are recorded.
        /// @param delta    The fixed step, in seconds; it sets the history's tick rate.
        /// @param context  Per-tick services; Role and IsReplay gate the recording, Tick labels it.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };

    /// @brief Moves every LagCompensated body to its pose at a past tick for as long as it lives.
    ///
    /// On construction it clamps @p viewTick into the scene's pose history (PoseHistory::ClampViewTick)
    /// and moves every recorded body except @p exclude to its reconstructed pose there
    /// (PoseHistory::RewoundPose), with a set-pose that wakes nothing. Queries made while it lives —
    /// Raycast, ShapeCast, Overlap — see the world as it stood at that tick. On destruction every
    /// moved body is restored bit for bit: pose, velocities and sleep state.
    ///
    /// - **What moves.** Only bodies the history recorded, on entities still carrying
    ///   LagCompensated. Static structure and unmarked bodies stay where they are. @p exclude — the
    ///   querying client's own entity — stays in the present, since its own view of itself is its
    ///   prediction, which the server's present matches.
    /// - **When nothing moves.** A view tick at or past the history's newest tick (a seat that draws
    ///   the present, such as a listen host's own), a scene with no history, and a history with no
    ///   closed tick all leave every body in place.
    /// - **Cost.** One state capture and set-pose per moved body on entry, one restore on exit. A
    ///   caller evaluating many queries from one client groups them under one scope per distinct
    ///   view tick.
    ///
    /// @warning A scope must not be held across a physics step: the step would simulate the rewound
    /// poses. Destroying a scope after its world stepped is a fatal assert.
    class VE_API RewindScope
    {
    public:
        /// @brief Rewinds the scene's LagCompensated bodies to @p viewTick.
        /// @param scene     The scene whose pose history and LagCompensated marks are read.
        /// @param world     The physics world the bodies live in (normally scene.GetPhysicsWorld()).
        /// @param viewTick  The (fractional) tick to rewind to; clamped into the history's reach.
        /// @param exclude   An entity to leave in the present, or Entity::Null.
        RewindScope(const Scene& scene, PhysicsWorld& world, f64 viewTick, Entity exclude);

        /// @brief Restores every moved body exactly.
        ~RewindScope();

        /// @brief A scope owns the restore of the bodies it moved; copying it would restore them twice.
        RewindScope(const RewindScope&) = delete;
        /// @brief A scope owns the restore of the bodies it moved; copying it would restore them twice.
        RewindScope& operator=(const RewindScope&) = delete;

        /// @brief Returns the tick the bodies stand at: @p viewTick clamped into the history's reach.
        [[nodiscard]] f64 GetViewTick() const { return m_ViewTick; }

        /// @brief Returns how many bodies the scope moved.
        [[nodiscard]] usize GetMovedCount() const { return m_Moved.size(); }

    private:
        /// @brief One moved body and the state it is restored to.
        struct MovedBody
        {
            /// @brief The body's entity.
            Entity Body;
            /// @brief Its state before the move (PhysicsWorld::SaveBodyState).
            vector<u8> State;
        };

        /// @brief The world the bodies were moved in.
        PhysicsWorld* m_World;
        /// @brief The world's step count on entry; a step before exit is misuse.
        u64 m_StepCount = 0;
        /// @brief The clamped tick the bodies stand at.
        f64 m_ViewTick = 0.0;
        /// @brief The bodies moved on entry, in move order.
        vector<MovedBody> m_Moved;
    };
}

VE_REFLECT(::Veng::LagCompensated, 0xB811FF2798DF6393ULL)
VE_REFLECT_END();

VE_SYSTEM(::Veng::PoseHistorySystem, 0x54E36BDEF5F90015ULL, "Pose History");
