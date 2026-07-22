#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Per-frame draw-budget accounting from the last Execute.
    ///
    /// The three gather phases lay out their draws through one shared slot budget and one shared
    /// skinning-palette budget; a frame whose candidates exceed either is clamped, and the
    /// submeshes past the limit are not drawn. This block is how many were lost and where, the
    /// per-frame number beside the once-per-renderer warning. Reached through
    /// SceneRenderer::GetDrawBudgetStats(); all zero before the first Execute.
    struct DrawBudgetStats
    {
        /// @brief The per-frame draw-slot budget in force, shared by all three gather phases.
        u32 SlotLimit = 0;
        /// @brief Slots actually claimed across the three phases this frame.
        u32 SlotsGranted = 0;
        /// @brief Submeshes the static opaque phase could not draw.
        ///
        /// The phase triages the skinned and translucent survivors as it lays out its own slots,
        /// so exhausting the budget also ends the triage: this counts every candidate left after
        /// the failed claim, including ones a later phase would otherwise have drawn.
        u32 StaticDropped = 0;
        /// @brief Submeshes the skinned phase could not draw for want of a draw slot.
        u32 SkinnedDropped = 0;
        /// @brief Submeshes the translucent phase could not draw.
        u32 TranslucentDropped = 0;
        /// @brief Skinned instances dropped for want of palette matrices.
        ///
        /// Counted per instance: the phase skips an instance whose palette does not fit and keeps
        /// gathering, since a later instance with fewer bones may still fit.
        u32 PaletteDropped = 0;
    };
}
