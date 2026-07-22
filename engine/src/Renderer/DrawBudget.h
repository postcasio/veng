#pragma once

#include <Veng/Renderer/DrawBudgetStats.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief The gather phase making a claim; names the counter a drop lands in.
    enum class DrawPhase : u8
    {
        /// @brief The static opaque phase, which lays out the slot range contiguous from 0.
        StaticOpaque,
        /// @brief The skinned phase, which claims a slot and its palette matrices together.
        Skinned,
        /// @brief The translucent phase, which lays out the last slots of the frame.
        Translucent,
    };

    /// @brief The outcome of a skinned draw's combined slot + palette claim.
    ///
    /// The two failures want different reactions: an instance whose palette does not fit is
    /// skipped (a later, smaller instance may still fit), while an exhausted slot budget ends
    /// the phase.
    enum class SkinnedClaim : u8
    {
        /// @brief Both the slot and the palette matrices were claimed.
        Granted,
        /// @brief The palette matrices did not fit; neither cursor moved.
        PaletteExhausted,
        /// @brief No draw slot remained; neither cursor moved.
        SlotsExhausted,
    };

    /// @brief Owns the per-frame draw-slot and palette-matrix budgets and the drop accounting.
    ///
    /// One budget serves all three gather phases, so the static opaque range stays contiguous
    /// from 0 and the phases cannot disagree about the limit. Device-free and I/O-free: it holds
    /// two cursors, two limits, and the per-phase drop counts, and never logs — the once-per-
    /// renderer warning is the renderer's, driven off GetStats().
    ///
    /// Indices handed out are **region-relative**, exactly as the cursors are: a caller folds a
    /// slot with its frame's DrawData base and a palette base with its frame's palette-region
    /// base at the write site.
    class DrawBudget
    {
    public:
        /// @brief Constructs an empty budget over the given per-frame limits.
        /// @param maxSlots           Draw slots a frame may lay out across all three phases.
        /// @param maxPaletteMatrices Skinning palette matrices a frame may write.
        DrawBudget(const u32 maxSlots, const u32 maxPaletteMatrices)
            : m_MaxSlots(maxSlots), m_MaxPaletteMatrices(maxPaletteMatrices)
        {
            m_Stats.SlotLimit = maxSlots;
        }

        /// @brief Claims the next draw slot.
        /// @param outSlot Receives the region-relative slot index on success; untouched on failure.
        /// @return True when a slot was claimed; false when the slot budget is exhausted.
        [[nodiscard]] bool TryClaimSlot(u32& outSlot)
        {
            if (m_SlotCursor >= m_MaxSlots)
            {
                return false;
            }
            outSlot = m_SlotCursor;
            ++m_SlotCursor;
            m_Stats.SlotsGranted = m_SlotCursor;
            return true;
        }

        /// @brief Claims a skinned draw's slot and its boneCount palette matrices together.
        ///
        /// All-or-nothing: on either failure neither cursor moves, so a slot is never burned for
        /// a draw that is not laid out and a palette base is never handed out for a draw that
        /// never happens. The palette is tested first, so an instance too large for the remaining
        /// palette is reported as such even on a frame whose slots are also gone.
        /// @param boneCount       Palette matrices this instance needs.
        /// @param outSlot         Receives the region-relative slot index on success.
        /// @param outPaletteBase  Receives the region-relative first palette matrix on success.
        /// @return Granted, or which budget refused the claim.
        [[nodiscard]] SkinnedClaim TryClaimSkinnedDraw(const u32 boneCount, u32& outSlot,
                                                       u32& outPaletteBase)
        {
            if (m_PaletteCursor + boneCount > m_MaxPaletteMatrices)
            {
                ++m_Stats.PaletteDropped;
                return SkinnedClaim::PaletteExhausted;
            }
            if (m_SlotCursor >= m_MaxSlots)
            {
                return SkinnedClaim::SlotsExhausted;
            }
            outSlot = m_SlotCursor;
            outPaletteBase = m_PaletteCursor;
            ++m_SlotCursor;
            m_PaletteCursor += boneCount;
            m_Stats.SlotsGranted = m_SlotCursor;
            return SkinnedClaim::Granted;
        }

        /// @brief Records candidates a phase abandoned after a claim failed.
        ///
        /// A phase stops at its first failed claim, so it reports the number of candidates left
        /// in its loop — the exact count, at no per-candidate cost.
        /// @param phase The phase whose counter the drops land in.
        /// @param count Candidates the phase abandoned, including the one whose claim failed.
        void RecordDropped(const DrawPhase phase, const u32 count)
        {
            switch (phase)
            {
            case DrawPhase::StaticOpaque:
                m_Stats.StaticDropped += count;
                break;
            case DrawPhase::Skinned:
                m_Stats.SkinnedDropped += count;
                break;
            case DrawPhase::Translucent:
                m_Stats.TranslucentDropped += count;
                break;
            }
        }

        /// @brief Returns the slots claimed so far, which is also the next slot to be handed out.
        [[nodiscard]] u32 GetSlotCursor() const { return m_SlotCursor; }

        /// @brief Returns the palette matrices claimed so far, region-relative.
        [[nodiscard]] u32 GetPaletteCursor() const { return m_PaletteCursor; }

        /// @brief Returns this frame's accounting: the limit, the grants, and the per-phase drops.
        [[nodiscard]] const DrawBudgetStats& GetStats() const { return m_Stats; }

    private:
        /// @brief Draw slots a frame may lay out across all three phases.
        u32 m_MaxSlots = 0;
        /// @brief Skinning palette matrices a frame may write.
        u32 m_MaxPaletteMatrices = 0;
        /// @brief Slots claimed so far, region-relative.
        u32 m_SlotCursor = 0;
        /// @brief Palette matrices claimed so far, region-relative.
        u32 m_PaletteCursor = 0;
        /// @brief The accounting handed back through GetStats().
        DrawBudgetStats m_Stats;
    };
}
