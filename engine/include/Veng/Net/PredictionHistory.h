#pragma once

#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/PredictionHistory.h — the per-tick ring of input + captured state, device-free.
//
// Prediction re-runs the real Sim systems on a small tracked set of entities ahead of the server,
// then reconciles against the authoritative snapshot: on a mismatch it restores the recorded state
// at the acknowledged tick and replays the recorded inputs forward. PredictionHistory is the store
// that makes that possible — a bounded ring keyed by client tick, each entry holding the seat input
// sampled that tick and a capture of every tracked entity's replicated component state. Capture is
// the reflection serializer (WriteFields) into pooled scratch, restore is ReadFields back over the
// live components, so no TypeInfo surface is added and the store is drift-proof against a component
// added or removed between ticks. It touches no socket and no time source; it is exercised entirely
// over an in-process Scene, the two-world-test idiom. Nothing here predicts — this is the machinery
// the prediction and reconciliation systems record into and roll back from.

namespace Veng
{
    class Scene;

    /// @brief Selects the client-side predicted entity set for a possessed pawn.
    ///
    /// The game policy hook the client's promotion consults on a possession change: given the pawn the
    /// local seat now controls, it returns the entities to promote to Tier::Predicted and track in the
    /// PredictionHistory (the prior set is demoted back to Remote and untracked). Unset falls back to
    /// the engine default — the pawn plus every descendant in its Hierarchy subtree that carries
    /// replicated state — which a game widens (a driven vehicle) or narrows through this hook.
    /// @param scene  The client scene the pawn and its subtree live in.
    /// @param pawn   The pawn the local seat now possesses (never null when invoked).
    /// @return The entities to predict; the pawn should generally be among them.
    using PredictionPolicy = function<vector<Entity>(const Scene& scene, Entity pawn)>;

    /// @brief The engine's default predicted set — what an unset PredictionPolicy falls back to.
    ///
    /// The pawn, plus every entity in its Hierarchy subtree that carries replicated state (a purely
    /// client-local view child carries none and is left Remote). Exposed so a game that narrows
    /// prediction for one world through PredictionPolicy can defer to the engine default for the
    /// rest, rather than reimplementing the owner-pawn-subtree walk to keep it.
    /// @param scene  The client scene the pawn and its subtree live in.
    /// @param pawn   The pawn to build the predicted set around.
    /// @return The pawn and its replicated descendants; empty when the pawn is null or dead.
    [[nodiscard]] VE_API vector<Entity> DefaultPredictedEntities(const Scene& scene, Entity pawn);

    namespace Net
    {
        /// @brief One recorded seat input keyed by the client tick it was sampled on — the replay source.
        struct StoredInput
        {
            /// @brief The client sim tick this input was sampled on.
            u64 Tick = 0;
            /// @brief The resolved seat input for that tick.
            PlayerInput Input;
        };

        /// @brief A bounded per-tick ring of seat input and captured tracked-entity state.
        ///
        /// Record captures the tracked set's replicated component state for a client tick alongside
        /// that tick's seat input; Restore rewinds the live scene to a recorded tick's captured state;
        /// InputsAfter is the ascending input run a replay steps forward; TrimThrough drops history the
        /// server has confirmed. The ring is bounded (Capacity): a Record past it trims the oldest
        /// entry and logs, so the store never grows without bound. The captured scratch is pooled and
        /// reused across trims rather than reallocated per tick. Ticks are recorded in ascending order.
        class VE_API PredictionHistory
        {
        public:
            /// @brief Ring sizing.
            struct Settings
            {
                /// @brief Maximum recorded ticks retained; a Record past it trims the oldest.
                ///
                /// Defaults to twice the worst supported round trip in ticks, so a mispredict at the
                /// oldest still-unacked tick has its inputs to replay.
                usize Capacity = 128;
            };

            /// @brief Constructs a history with the default capacity.
            PredictionHistory() = default;

            /// @brief Constructs a history with the given sizing.
            /// @param settings  The ring sizing.
            explicit PredictionHistory(const Settings& settings) : m_Settings(settings) {}

            /// @brief Adds an entity to the tracked (predicted) set; a no-op if already tracked.
            /// @param entity  The entity whose replicated state is captured each Record.
            void Track(Entity entity);

            /// @brief Removes an entity from the tracked set; a no-op if not tracked.
            ///
            /// Already-recorded captures of the entity are left intact — a restore of an earlier tick
            /// still rewinds it; new records simply stop capturing it.
            /// @param entity  The entity to stop capturing.
            void Untrack(Entity entity);

            /// @brief The tracked (predicted) entity set, in insertion order.
            [[nodiscard]] std::span<const Entity> Tracked() const { return m_Tracked; }

            /// @brief Captures the tracked set's replicated state and this tick's input for @p tick.
            ///
            /// For each tracked entity alive in @p scene, serializes every Replicated-marked component
            /// present on it (WriteFields into pooled scratch); Local/View state is never captured. The
            /// tick must exceed the newest recorded tick (append), or equal it (overwrite the newest).
            /// Recording past the capacity trims the oldest entry first and logs a warning.
            /// @param tick   The client sim tick being recorded (ascending across calls).
            /// @param input  The seat input resolved for this tick.
            /// @param scene  The scene the tracked entities' state is captured from.
            void Record(u64 tick, const PlayerInput& input, const Scene& scene);

            /// @brief Restores the tracked entities' captured state at @p tick over the live scene.
            ///
            /// For each captured entity still alive in @p scene, ReadFields each captured component back
            /// over the live component (adding an absent one), and removes any Replicated component now
            /// on the entity that the capture did not hold — so the entity's replicated state matches
            /// the recorded tick exactly, tolerant of a component added or removed since. A captured
            /// entity no longer alive is skipped. Non-replicated (Local/View) state is untouched.
            /// @param tick   The recorded tick to rewind to.
            /// @param scene  The scene to restore into.
            /// @return True if @p tick was recorded and restored, false if it was not in the ring.
            [[nodiscard]] bool Restore(u64 tick, Scene& scene) const;

            /// @brief The captured bytes of @p type on @p entity at recorded @p tick, or empty when absent.
            ///
            /// The reconciler's compare source: the WriteFields serialization of the entity's
            /// component as predicted at @p tick, to compare field-wise against the authoritative
            /// snapshot record. Empty when @p tick is not recorded, the entity was not captured that
            /// tick, or it held no such component. The view is valid until the next mutating call.
            /// @param tick    The recorded tick to read.
            /// @param entity  The captured entity.
            /// @param type    The component type to read.
            /// @return The captured component bytes, or an empty span when absent.
            [[nodiscard]] std::span<const u8> Captured(u64 tick, Entity entity, TypeId type) const;

            /// @brief The recorded inputs for every tick strictly after @p tick, in ascending order.
            ///
            /// The replay source after a reconciliation: restore at the acked tick, then step these
            /// inputs forward through the Sim systems. The view is valid until the next mutating call.
            /// @param tick  The exclusive lower bound; inputs with Tick > tick are returned.
            /// @return A contiguous ascending view of the matching stored inputs.
            [[nodiscard]] std::span<const StoredInput> InputsAfter(u64 tick) const;

            /// @brief Drops every recorded tick at or before @p tick — history the server has confirmed.
            /// @param tick  The highest confirmed tick; entries with Tick <= tick are dropped.
            void TrimThrough(u64 tick);

            /// @brief Drops all recorded ticks, keeping the tracked set and pooled scratch.
            void Clear();

            /// @brief The number of recorded ticks currently retained.
            [[nodiscard]] usize Size() const { return m_Inputs.size(); }

            /// @brief True if @p tick is currently recorded.
            [[nodiscard]] bool Contains(u64 tick) const;

            /// @brief The oldest recorded tick, or 0 when empty.
            [[nodiscard]] u64 OldestTick() const;

            /// @brief The newest recorded tick, or 0 when empty.
            [[nodiscard]] u64 NewestTick() const;

        private:
            /// @brief One tracked entity's captured replicated components at a tick.
            struct CapturedComponent
            {
                /// @brief The component's reflected type.
                TypeId Type = 0;
                /// @brief The WriteFields serialization of the component's state.
                vector<u8> Bytes;
            };

            /// @brief One tracked entity's capture at a tick.
            struct CapturedEntity
            {
                /// @brief The captured entity handle (resolved against the live scene on restore).
                Entity Entity;
                /// @brief The entity's replicated components at the recorded tick.
                vector<CapturedComponent> Components;
            };

            /// @brief One recorded tick's captured state (parallel to the same index in m_Inputs).
            struct Frame
            {
                /// @brief The tracked entities captured this tick.
                vector<CapturedEntity> Entities;
            };

            /// @brief Captures the tracked set into @p frame, reusing its buffers, from @p scene.
            void CaptureInto(Frame& frame, const Scene& scene) const;

            /// @brief Retires @p count oldest entries to the pool (front of the parallel rings).
            void RetireOldest(usize count);

            /// @brief Ring sizing.
            Settings m_Settings;
            /// @brief The tracked (predicted) entities.
            vector<Entity> m_Tracked;
            /// @brief The recorded inputs, ascending by tick — contiguous for InputsAfter's span.
            vector<StoredInput> m_Inputs;
            /// @brief The captured state per recorded tick, parallel to m_Inputs by index.
            vector<Frame> m_Frames;
            /// @brief Retired frame buffers kept for reuse, so a steady ring reallocates nothing.
            vector<Frame> m_Pool;
        };
    }
}
