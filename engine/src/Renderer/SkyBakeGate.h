#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Decides whether a baked material sky must (re-)bake its radiance cube this resolve.
    ///
    /// A device-free pure decision, so the gate — whose two failure modes each shipped as a live bug
    /// before it was pinned — is unit-tested rather than only exercised on the GPU. The `SkyResolver`
    /// calls it once per resolve of a baked material sky.
    ///
    /// The gate has two regimes, selected by whether the caller supplied a nonzero **content key**:
    ///
    /// - **Keyed (`bakeKey != 0`):** the key wins over material identity, so two worlds authoring
    ///   distinct instances of equal content share one bake — a world swap re-authoring an
    ///   equal-content material costs no re-bake. The key and the cube's validity survive a transient
    ///   no-sky gap (a world swap before the destination authors its `Sky`), so an equal-key sky
    ///   returns to its standing cube without re-baking. The second clause — no valid cube and none
    ///   outstanding — fires the first bake and re-fires after an abandoned one, but is gated on
    ///   `bakeOutstanding` (Pending **or** the one-frame Landed gap before the copy) so it never
    ///   supersedes an in-flight fill, which would leave the display cube perpetually unfilled.
    /// - **Unkeyed (`bakeKey == 0`):** the historical gate — re-bake on a material-instance swap or an
    ///   in-place revision bump. This is the path every authored sky takes (the key defaults to 0).
    ///
    /// @param bakeKey         The resolved MaterialSky's content key this frame (0 = unkeyed).
    /// @param lastBakeKey     The key the display cube was last baked for (0 before any keyed bake).
    /// @param displayCubeValid Whether the display cube holds a landed bake.
    /// @param bakeOutstanding Whether an amortized bake is filling or landed-but-not-yet-copied.
    /// @param material        The resolved material instance (identity only; never dereferenced here).
    /// @param lastMaterial    The instance the cube was last baked from (identity only).
    /// @param revision        The resolved material's revision this frame.
    /// @param lastRevision    The revision the cube was last baked from.
    /// @return True when a (re-)bake must be requested this resolve.
    [[nodiscard]] inline bool ShouldRebakeMaterialSky(u64 bakeKey, u64 lastBakeKey,
                                                      bool displayCubeValid, bool bakeOutstanding,
                                                      const void* material,
                                                      const void* lastMaterial, u32 revision,
                                                      u32 lastRevision)
    {
        if (bakeKey != 0)
        {
            return bakeKey != lastBakeKey || (!displayCubeValid && !bakeOutstanding);
        }
        return material != lastMaterial || revision != lastRevision;
    }
}
