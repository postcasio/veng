#pragma once

#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace VengEditor
{
    /// @brief Serialises one asset editor's recooks: one in flight, at most one queued.
    ///
    /// A cook already in flight read an older source, so a recook requested behind it must run
    /// once that one lands — dropping it would leave the mounted asset behind the file on disk.
    /// Only the most recent queued request is kept: two queued requests would cook the same
    /// source twice, so the later supersedes the earlier.
    class CookGate
    {
    public:
        /// @brief Submits a recook now, or holds it until the cook in flight completes.
        ///
        /// @param submit  Submits the cook. It owns calling Complete() when the whole operation
        ///                finishes — including a failure that never reaches a backend, and the
        ///                second half of a chained cook — or the gate stays shut.
        void Request(Veng::function<void()> submit);

        /// @brief Releases the gate and runs the queued request, if there is one.
        void Complete();

        /// @brief Returns true while a cook submitted through this gate is in flight.
        [[nodiscard]] bool IsCooking() const { return m_Cooking; }

    private:
        /// @brief The one held request, run by Complete(); empty when nothing is queued.
        Veng::function<void()> m_Queued;
        /// @brief Whether a submitted cook has yet to report completion.
        bool m_Cooking = false;
    };

    /// @brief Runs an asset editor's explicit-save sequence: write, clear dirty, then recook.
    ///
    /// The order is the contract every asset editor's Save() is built on. The cook reads the file
    /// the write just produced, so a cook that fails leaves the saved source on disk and reports
    /// in-panel; a write that fails clears no dirty flag and cooks nothing, so the editor still
    /// holds the edits it could not persist and the mounted asset still matches the file.
    /// @param write   Writes the document to its source.
    /// @param dirty   The panel's unsaved-changes flag, cleared only once the write succeeds.
    /// @param recook  Submits the recook of the written source.
    /// @return Empty on success; the write's error otherwise.
    [[nodiscard]] Veng::VoidResult SaveAssetSource(const Veng::function<Veng::VoidResult()>& write,
                                                   bool& dirty,
                                                   const Veng::function<void()>& recook);
}
