#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    class Store;
    class WorldRunner;

    /// @brief A checkpoint's measured costs in milliseconds.
    struct CheckpointCost
    {
        /// @brief The last checkpoint's capture, on the calling thread.
        f64 CaptureMs = 0.0;
        /// @brief The last checkpoint's flush call on the calling thread: the snapshot for a
        ///        background flush, the snapshot and the whole write for a waited one.
        f64 FlushMs = 0.0;
        /// @brief The store's most recent write, measured on the job that ran it.
        f64 WriteMs = 0.0;
    };

    /// @brief The timed and on-demand whole-store checkpoint over a runner's live worlds.
    ///
    /// The consumer-side cadence every store-backed application otherwise re-implements: on the
    /// interval (and on demand), capture every live world's persistent state into the store, then
    /// flush the slot atomically. The timed and on-demand checkpoint starts a background flush, so
    /// the frame pays the capture and the flush's snapshot, not the file I/O; CheckpointAndWait is
    /// the full, blocking form for the exit path and an explicit save. The store resolves per
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

        /// @brief Captures every live world's persistent state into the store, then starts a
        ///        background flush.
        ///
        /// Capture is memory-only and main-thread; the flush takes its snapshot here and writes on
        /// a background job (Store::Flush), honouring family flush intervals. It does not wait for
        /// the write; a failed write is logged by the store and its families retry at the next
        /// flush. A no-op when the source resolves no store.
        void CheckpointNow();

        /// @brief Captures every live world's persistent state, then flushes fully and waits.
        ///
        /// Every dirty family is written whatever its interval, and the write has committed when
        /// this returns (Store::FlushAndWait). For the exit path, a slot close, and an explicit
        /// save. A failed write is logged and returned. A no-op when the source resolves no store.
        /// @return Empty on success or with no store; the failed write's error otherwise.
        VoidResult CheckpointAndWait();

        /// @brief The last checkpoint's measured costs in milliseconds.
        ///
        /// WriteMs is read from the store the source resolves now, and is 0 with none.
        [[nodiscard]] CheckpointCost LastCostMs() const;

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
        /// @brief The last checkpoint's flush call cost on the calling thread, in milliseconds.
        f64 m_LastFlushMs = 0.0;

        /// @brief Captures every live world into the store, then runs the flush half.
        /// @return The flush half's result.
        VoidResult Run(Store& store, bool wait);
    };
}
