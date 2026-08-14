#pragma once

#include <Veng/Veng.h>

#include <utility>

namespace Veng
{
    class Store;
    class WorldRunner;

    /// @brief The timed and on-demand whole-store checkpoint over a runner's live worlds.
    ///
    /// The consumer-side cadence every store-backed application otherwise re-implements: on the
    /// interval (and on demand — a save action, the exit path), capture every live world's
    /// persistent state into the store, then flush the slot atomically. The store resolves per
    /// checkpoint through the source, so a process whose store opens and closes at runtime (a
    /// save-slot switch) needs no rebinding — a source resolving null makes the checkpoint a no-op.
    ///
    /// The two halves carry the "Checkpoint/Capture" and "Checkpoint/Flush" profiler scopes, and
    /// their wall-clock costs are also kept as plain state (LastCostMs): a cost readout drawn by a
    /// panel must hold a value in every build configuration, which the profiler's per-frame
    /// aggregates cannot carry — they zero for a frame the scope did not run in, and do not exist
    /// under VE_PROFILE=OFF.
    class VE_API StoreCheckpoint
    {
    public:
        /// @brief What the checkpoint runs over.
        struct Info
        {
            /// @brief The runner whose live worlds every checkpoint captures; borrowed.
            WorldRunner* Runner = nullptr;
            /// @brief Resolves the store to capture into and flush; null-returning is a no-op.
            function<Store*()> StoreSource;
            /// @brief Seconds between timed checkpoints (Update's cadence).
            f64 IntervalSeconds = 60.0;
        };

        /// @brief Builds the checkpoint; nothing runs until Update accrues or CheckpointNow is called.
        explicit StoreCheckpoint(Info info);

        /// @brief Advances the timed cadence; on the interval, runs CheckpointNow.
        ///
        /// The accumulator only advances while the source resolves a store, so a store opened after
        /// a long storeless stretch is not immediately checkpointed by the backlog.
        /// @param delta  The frame's wall-clock step in seconds.
        void Update(f32 delta);

        /// @brief Captures every live world's persistent state into the store, then flushes the slot.
        ///
        /// Capture is memory-only and main-thread; the flush is the file I/O, atomic per the
        /// store's whole-slot contract. A failed flush is logged — the records stay captured and
        /// the next flush retries them. A no-op when the source resolves no store.
        void CheckpointNow();

        /// @brief The last checkpoint's measured (capture, flush) cost in milliseconds.
        [[nodiscard]] std::pair<f64, f64> LastCostMs() const
        {
            return {m_LastCaptureMs, m_LastFlushMs};
        }

    private:
        /// @brief The runner whose worlds are captured.
        WorldRunner& m_Runner;
        /// @brief Resolves the store per checkpoint; null-returning is the no-op posture.
        function<Store*()> m_StoreSource;
        /// @brief Seconds between timed checkpoints.
        f64 m_IntervalSeconds;
        /// @brief The accumulator driving the timed checkpoint.
        f64 m_Accumulator = 0.0;
        /// @brief The last checkpoint's measured capture cost in milliseconds.
        f64 m_LastCaptureMs = 0.0;
        /// @brief The last checkpoint's measured flush cost in milliseconds.
        f64 m_LastFlushMs = 0.0;
    };
}
