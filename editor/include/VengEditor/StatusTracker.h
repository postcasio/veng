#pragma once

#include <Veng/Veng.h>

namespace VengEditor
{
    /// @brief Tracks the in-flight long-running editor tasks the status bar reports.
    ///
    /// A long-running editor operation (a cook, a batch import) brackets itself with
    /// Begin/End; the status bar reads a GetSnapshot() each frame to show a description and a
    /// progress bar. Progress across several tasks is **count-based** — the editor's tasks
    /// (cooks) report no sub-step progress, so the tracker measures completion as finished of
    /// total within the current wave (a run of overlapping tasks bounded by the tracker
    /// returning to idle), not as a fraction of any one task's internal work.
    ///
    /// All calls run on the main thread — Begin from the request site, End from the task's
    /// main-thread continuation, GetSnapshot from the render frame — so the tracker needs no
    /// synchronization, matching the engine's single-render-thread model.
    class StatusTracker
    {
    public:
        /// @brief Opaque handle identifying one tracked task, returned by Begin.
        using TaskId = Veng::u64;

        /// @brief Registers a running task and returns its id.
        ///
        /// Starting a task while none are active begins a new wave (the completed/total
        /// counters reset). The returned id is passed to End when the task finishes.
        /// @param description  Short human-readable label shown while this task runs.
        /// @return The id identifying the task, to pass to End.
        TaskId Begin(Veng::string description);

        /// @brief Marks a tracked task complete and removes it from the active set.
        ///
        /// Increments the current wave's completed count. An unknown id (already ended) is
        /// ignored. When the last active task ends the wave is finished; the next Begin starts
        /// a fresh one.
        /// @param id  The id returned by the matching Begin call.
        void End(TaskId id);

        /// @brief Immutable view of the tracker's state for one render frame.
        struct Snapshot
        {
            /// @brief Descriptions of the tasks running right now, oldest first.
            ///
            /// Empty means idle. The size is the running-task count; the front entry is the
            /// label the collapsed status bar shows for a single task.
            Veng::vector<Veng::string> Tasks;
            /// @brief Tasks finished since the current wave began.
            Veng::u32 CompletedInWave = 0;
            /// @brief Tasks begun since the current wave began.
            Veng::u32 TotalInWave = 0;
        };

        /// @brief Returns a snapshot of the active tasks and the current wave's progress.
        [[nodiscard]] Snapshot GetSnapshot() const;

    private:
        /// @brief One active task: its id and the description the bar shows.
        struct Entry
        {
            /// @brief The task's id, matched by End.
            TaskId Id = 0;
            /// @brief The short label shown while the task runs.
            Veng::string Description;
        };

        /// @brief The currently running tasks, oldest first.
        Veng::vector<Entry> m_Active;
        /// @brief Monotonic id source; never reused within a run.
        TaskId m_NextId = 1;
        /// @brief Tasks completed in the current wave.
        Veng::u32 m_Completed = 0;
        /// @brief Tasks begun in the current wave.
        Veng::u32 m_Total = 0;
    };
}
