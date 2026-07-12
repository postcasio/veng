#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/ViewportId.h>

namespace Veng::Renderer
{
    class Viewport;

    /// @brief Non-owning ViewportId to Viewport registry that resolves an id to the live viewport.
    ///
    /// The render-domain identity registry joining BindlessRegistry on the Context: every Viewport
    /// mints an id here at Create and retires it at destruction, so the map's maintenance window is
    /// the viewport's construction lifetime — broader than any drive-list membership, so a live but
    /// unregistered viewport still resolves. The registry never owns a viewport (its Unique lives
    /// elsewhere) and never extends its life; the "resolve live, borrow only within a frame"
    /// discipline is what keeps a resolved pointer safe.
    ///
    /// Standalone-constructible: its whole state is a monotonic counter, a per-instance salt, and
    /// the map, so it needs no Context, device, or ICD and can be default-constructed on its own.
    /// The counter is an instance member — one Context drives one render thread, so there is no
    /// synchronization and no cross-instance leakage. Each instance mixes a distinct salt into every
    /// id it mints and checks it on Resolve, so an id minted by a foreign registry resolves to
    /// nullptr rather than to a same-numbered stranger.
    class ViewportRegistry
    {
    public:
        /// @brief Constructs an empty registry with a fresh per-instance salt.
        ViewportRegistry();

        /// @brief Mints a fresh, never-reused id for @p viewport and records the mapping.
        ///
        /// The id carries this registry's salt in its high bits and the next monotonic counter value
        /// in its low bits, so it is unique within this registry and separable from any foreign
        /// registry's ids. The viewport's address is stored, never owned.
        /// @param viewport  The viewport to register.
        /// @return The minted id, valid for the viewport's lifetime.
        [[nodiscard]] ViewportId Mint(Viewport& viewport);

        /// @brief Drops the mapping for @p id.
        ///
        /// Retiring an invalid, unknown, or already-retired id is a no-op.
        /// @param id  The id to retire.
        void Retire(ViewportId id);

        /// @brief Resolves @p id to the live viewport, or nullptr when it names none.
        ///
        /// Returns nullptr for an invalid, unminted, retired, or foreign-registry id — the salt
        /// check rejects a foreign id before the lookup. O(1).
        /// @param id  The id to resolve.
        /// @return The live viewport, or nullptr.
        [[nodiscard]] const Viewport* Resolve(ViewportId id) const;

    private:
        /// @brief Per-instance salt occupying the high bits of every minted id.
        u64 m_Salt = 0;
        /// @brief Monotonic mint counter; the low bits of each id, never reused.
        u64 m_Counter = 0;
        /// @brief The live id-value to viewport mapping, maintained across each viewport's lifetime.
        unordered_map<u64, Viewport*> m_Viewports;
    };
}
