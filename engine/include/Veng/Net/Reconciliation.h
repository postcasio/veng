#pragma once

#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/Replication.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Veng.h>

#include <functional>
#include <span>

// Veng/Net/Reconciliation.h — the client-side compare/restore/replay that converges prediction to
// authoritative truth. A snapshot's LastConsumedInputTick C tells the client which of its inputs the
// server had consumed; the client compares its recorded prediction at C (PredictionHistory) against
// the authoritative record. On a match the prediction stands and history through C is trimmed; on a
// mismatch the predicted set is restored to the authoritative record at C and the recorded inputs
// C+1..now are replayed through the real Sim systems (re-recording history), then the visual residual
// is eased out through a decaying render offset (never written into sim Transform). Device-free: it
// drives the replay through a caller-supplied callback (which runs the scene's Sim phase with
// SystemContext::IsReplay set), so it unit-tests over an in-process Scene the two-world-test way.

namespace Veng
{
    class Scene;

    namespace Net
    {
        /// @brief The per-leaf tolerances and smoothing knobs the reconciler compares/corrects with.
        struct ReconcileTolerances
        {
            /// @brief Max per-component position drift (meters) a spatial leaf may differ by and still match.
            ///
            /// Sub-epsilon drift is carried, not chased: the next mismatch that exceeds it corrects
            /// from authoritative truth, so float divergence cannot accumulate unboundedly.
            f32 Position = 0.01f;
            /// @brief Max rotation drift (1 - |dot(q_auth, q_pred)|) a quaternion leaf may differ by and still match.
            f32 Rotation = 1.0e-4f;
            /// @brief A correction whose position residual exceeds this (meters) snaps instead of smoothing.
            ///
            /// A teleport-scale correction should not ease over 100+ ms across the screen; past this
            /// the render offset is dropped and the pose snaps to the corrected position at once.
            f32 SnapDistance = 4.0f;
        };

        /// @brief Field-aware equality: spatial leaves within tolerance, every other leaf exact.
        ///
        /// Walks @p info recursively — a Vector leaf (position/scale) compares per-component within
        /// @p tol.Position, a Quaternion leaf within @p tol.Rotation, a nested struct/variant/array
        /// recurses, and every discrete leaf (scalar, enum, string, reference, asset handle, matrix)
        /// compares exactly. So a spatial pose matches within epsilon while discrete gameplay state
        /// must match bit-for-bit. Both values are in local form (references already remapped).
        /// @param a         The authoritative value.
        /// @param b         The predicted value.
        /// @param info      The reflected type of both values.
        /// @param registry  The registry resolving nested field types.
        /// @param tol       The per-leaf tolerances.
        /// @return True when the values match under the tolerance policy.
        [[nodiscard]] VE_API bool ValuesMatch(const void* a, const void* b, const TypeInfo& info,
                                              const TypeRegistry& registry,
                                              const ReconcileTolerances& tol);

        /// @brief Replays one predicted Sim tick during rollback: advances the scene's Sim phase for @p tick.
        ///
        /// Called for each recorded input in C+1..now on a mispredict, in ascending order. The
        /// implementer sets the local seat's PlayerInput to @p input and advances the scene's Sim
        /// phase for @p tick with SystemContext::IsReplay set (so a system with outward side effects
        /// gates them), re-running control + movement over the predicted set exactly as the live
        /// client tick did. An empty callback disables rollback — a mispredict then hard-snaps to the
        /// authoritative state.
        using ReplayTick = function<void(Scene& scene, u64 tick, const PlayerInput& input)>;

        /// @brief The outcome of a reconciliation pass, for the caller and for tests.
        struct ReconcileResult
        {
            /// @brief True when the pass had an authoritative record to compare against.
            bool Compared = false;
            /// @brief True when a mismatch was found (or a degenerate case forced a correction).
            bool Corrected = false;
            /// @brief True when the correction snapped (history underflow, no replay, or beyond the snap distance).
            bool Snapped = false;
            /// @brief The number of recorded inputs replayed forward on a rollback.
            u32 ReplayedTicks = 0;
        };

        /// @brief Reconciles the predicted set against a snapshot's authoritative records at tick C.
        ///
        /// On a match (the recorded prediction at @p consumedTick equals the authoritative record
        /// under @p tol) the prediction stands and history through @p consumedTick is trimmed. On a
        /// mismatch the predicted set is restored to the authoritative record and @p replay steps the
        /// recorded inputs C+1..now forward (re-recording history), then the pre-correction visible
        /// pose is held as a decaying PredictionError render offset (unless the correction exceeds
        /// @p tol.SnapDistance, when it snaps). A history underflow (@p consumedTick older than the
        /// ring, or no @p replay) restores the authoritative present, clears history, and re-predicts
        /// from live input — never a crash.
        /// @param scene          The client scene the predicted set lives in.
        /// @param history        The prediction history to compare, restore, and re-record through.
        /// @param authoritative  The snapshot's authoritative records for the predicted entities.
        /// @param consumedTick   The client input tick the snapshot's state reflects (its C).
        /// @param replay         The per-tick Sim replay driver (empty disables rollback).
        /// @param tol            The compare tolerances and smoothing knobs.
        /// @return What the pass did (see ReconcileResult).
        VE_API ReconcileResult Reconcile(Scene& scene, PredictionHistory& history,
                                         std::span<const PredictedRecord> authoritative,
                                         u64 consumedTick, const ReplayTick& replay,
                                         const ReconcileTolerances& tol);
    }
}
