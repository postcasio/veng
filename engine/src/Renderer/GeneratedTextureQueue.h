#pragma once

// The generated-texture service's scheduling core, separated from the GPU half so the policy —
// idempotent keys, priority-then-FIFO selection, cost-budget accounting across mixed jobs — is a
// pure function of the job set and is unit-testable with no ICD (the FrameTopology / DrawBudget
// precedent). It records nothing, owns no resource, and knows nothing about what a tick does.

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Where a generated-texture job stands in its lifecycle.
    enum class GeneratedTextureState : u8
    {
        /// @brief Requested; no tick has run yet.
        Queued,
        /// @brief At least one tick has run and at least one remains.
        Running,
        /// @brief Every tick has run; the targets are filled and sampleable.
        Resident,
    };

    /// @brief One job's scheduling record: its identity, its order, and its progress.
    struct GeneratedTextureJobRecord
    {
        /// @brief The caller's key; unique across live jobs.
        u64 Key = 0;
        /// @brief Higher runs first. Raisable while the job is Queued or Running.
        i32 Priority = 0;
        /// @brief Request order, breaking priority ties so equal-priority jobs run FIFO.
        u64 Sequence = 0;
        /// @brief Total ticks the job takes to fill its targets.
        u32 TickCount = 1;
        /// @brief Ticks run so far.
        u32 TicksDone = 0;
        /// @brief The cost one of this job's ticks charges against the per-frame budget.
        ///
        /// A relative weight the caller declares, not a measured time: a job whose single tick is
        /// expensive (a heavy fragment, a long dispatch) sets it above the baseline of one, so the
        /// budget spends fewer of its ticks per frame. At the default of one, a budget of N spends N
        /// ticks per frame — the tick-count schedule this replaced.
        u32 Cost = 1;
        /// @brief The job's lifecycle state.
        GeneratedTextureState State = GeneratedTextureState::Queued;
        /// @brief Whether the job is live but excluded from selection, so no tick is spent on it.
        ///
        /// A job whose result may already exist elsewhere is held while that is being established:
        /// the key is live (so a re-request is still idempotent and the job counts as pending), but
        /// running a tick would spend the work the answer might make unnecessary.
        bool Held = false;
    };

    /// @brief Priority-then-FIFO scheduling over a keyed job set, spending a per-frame cost budget.
    ///
    /// The selection rule is the whole policy: among the jobs with ticks left, the one with the
    /// highest priority wins, ties broken by request order. It is re-evaluated per tick, so raising
    /// a queued job's priority preempts a running one at the next tick rather than at the next job.
    class GeneratedTextureQueue
    {
    public:
        /// @brief A budget meaning "run every pending tick this pump".
        static constexpr u32 UnlimitedCost = ~0u;

        /// @brief Adds a job, idempotently on its key.
        ///
        /// A key already present — queued, running, or resident — is left exactly as it is, so a
        /// caller re-requesting each frame while a body is in its prefetch band neither restarts
        /// the job nor duplicates it.
        /// @param key       The caller's key.
        /// @param tickCount Ticks the job takes; zero is treated as one.
        /// @param priority  Initial priority; higher runs first.
        /// @param cost      Cost one of the job's ticks charges against a pump's budget; zero is
        ///                  treated as one, and one — the default — reproduces the tick-count schedule.
        /// @return True when the job was added; false when the key was already live.
        bool Add(u64 key, u32 tickCount, i32 priority, u32 cost = 1);

        /// @brief Removes a job whatever its state.
        /// @param key The job's key.
        /// @return True when a job was removed.
        bool Remove(u64 key);

        /// @brief Repriorities a live job. A resident job's priority is inert but still recorded.
        /// @param key      The job's key.
        /// @param priority The new priority.
        /// @return True when a job was found.
        bool SetPriority(u64 key, i32 priority);

        /// @brief Excludes a job from selection, or restores it, leaving its progress untouched.
        /// @param key   The job's key.
        /// @param held  True to exclude the job from selection, false to make it selectable again.
        /// @return True when a job was found.
        bool SetHeld(u64 key, bool held);

        /// @brief Marks a job resident without spending its ticks, for a result obtained elsewhere.
        ///
        /// The job's remaining ticks are abandoned, not run: its targets were filled by something
        /// other than its tick callback, so running one would overwrite the result.
        /// @param key The job's key.
        /// @return True when a job that was not already resident was marked.
        bool MarkResident(u64 key);

        /// @brief The record for a key, or null when the key is not live.
        [[nodiscard]] const GeneratedTextureJobRecord* Find(u64 key) const;

        /// @brief Whether a key is live (queued, running, or resident).
        [[nodiscard]] bool Contains(u64 key) const { return Find(key) != nullptr; }

        /// @brief Every live job's record, in insertion order.
        [[nodiscard]] const vector<GeneratedTextureJobRecord>& GetJobs() const { return m_Jobs; }

        /// @brief The number of jobs that are not yet resident, held ones included.
        [[nodiscard]] u32 GetPendingCount() const;

        /// @brief The number of jobs a tick could be spent on right now.
        [[nodiscard]] u32 GetSelectableCount() const;

        /// @brief The number of jobs whose ticks are exhausted.
        [[nodiscard]] u32 GetResidentCount() const;

        /// @brief The key of the job the next tick belongs to, or nullopt when nothing is pending.
        [[nodiscard]] optional<u64> NextKey() const;

        /// @brief Spends up to @p budget of summed tick cost across the pending jobs, in order.
        ///
        /// Runs ticks in selection order, charging each job's Cost against the budget, and stops once
        /// the next tick's cost would exceed what remains. The first tick of a pump always runs when
        /// the budget is a positive, untouched one, so a single tick dearer than the whole budget
        /// still advances one-a-frame instead of stalling forever; a budget of zero spends nothing.
        /// Invokes @p tick once per tick with the job's key, the index of the tick within that job,
        /// and the job's total; then, on the tick that exhausts a job, @p complete with its key. A
        /// budget of UnlimitedCost runs every pending tick. Neither callback may add or remove jobs —
        /// the caller defers any such reaction until Spend has returned.
        /// @param tick     Invoked per tick as (key, tickIndex, tickCount).
        /// @param complete Invoked once per job, on the tick that exhausts it.
        /// @param budget   Maximum summed tick cost to spend.
        /// @return The number of ticks run.
        u32 Spend(u32 budget, const function<void(u64, u32, u32)>& tick,
                  const function<void(u64)>& complete);

    private:
        /// @brief The mutable record for a key, or null when the key is not live.
        GeneratedTextureJobRecord* FindMutable(u64 key);

        /// @brief Live jobs, in insertion order.
        vector<GeneratedTextureJobRecord> m_Jobs;
        /// @brief Monotonic request counter feeding GeneratedTextureJobRecord::Sequence.
        u64 m_NextSequence = 0;
    };
}
