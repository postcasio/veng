#pragma once

#include <span>

#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Visibility.h>
#include <Veng/Veng.h>

#include "DrawPlan.h"
#include "GpuBlocks.h"

namespace Veng::Renderer
{
    /// @brief Packs an entity into the key the per-frame velocity and palette maps are keyed by.
    ///
    /// Index and generation both participate, so a reused slot never inherits the previous
    /// entity's previous-frame world or palette base.
    /// @param entity The entity to key by.
    /// @return The packed key.
    [[nodiscard]] constexpr u64 PackEntity(const Entity entity)
    {
        return (static_cast<u64>(entity.Index) << 32) | static_cast<u64>(entity.Generation);
    }

    /// @brief The per-frame inputs the three draw-gather phases share.
    ///
    /// Plain data plus the mapped write targets, bundled so each phase takes one const reference
    /// rather than a dozen positional parameters. The genuinely mutable state — the slot and
    /// palette cursors, the plans, the palette-base map — stays an explicit by-reference
    /// parameter, so mutation is visible at every call site.
    struct DrawGatherInput
    {
        /// @brief Every per-submesh candidate this frame; the survivor ids index it.
        std::span<const SubMeshCandidate> Candidates;
        /// @brief The frame's view: the world the skinned poses are read from and the camera the
        ///        translucent sort keys off.
        const SceneView& View;
        /// @brief First DrawData record of this frame's ring region; a slot writes at FrameBase + slot.
        u32 FrameBase = 0;
        /// @brief First palette matrix of this frame's ring region.
        u32 PaletteRegionBase = 0;
        /// @brief Slots a frame may lay out across all three phases; the overflow is dropped.
        u32 MaxSlots = 0;
        /// @brief Palette matrices a frame may write; an instance that would exceed it is skipped.
        u32 MaxPaletteMatrices = 0;
        /// @brief Mapped per-draw DrawData records the phases write through.
        GpuDrawData* DrawData = nullptr;
        /// @brief Mapped GPU-cull candidate records for this frame's region, or null under CPU culling.
        GpuCullCandidate* CullData = nullptr;
        /// @brief Mapped skinning palette matrices; a palette base indexes this absolutely.
        mat4* PaletteData = nullptr;
        /// @brief Previous frame's world matrix per packed entity, the object-velocity source.
        const unordered_map<u64, mat4>& PreviousWorlds;
        /// @brief Previous frame's palette base per packed entity, the deformation-velocity source.
        const unordered_map<u64, u32>& PreviousPaletteBases;
    };

    /// @brief Groups contiguous slots sharing both a source mesh and a pipeline.
    ///
    /// The mesh's buffers and the material pipeline each bind once per group. Splitting on the
    /// pipeline (not just the mesh) is what lets surface materials with different fragment shaders
    /// coexist — each group binds its own. Pure: a span of slots in, groups appended out.
    /// @param slots  The slots to group, in submission order.
    /// @param groups Receives one group per contiguous run; appended to, not cleared.
    void GroupContiguousSlots(std::span<const DrawSlot> slots, vector<DrawGroup>& groups);

    /// @brief Lays out the static opaque slots and triages the survivors the later phases gather.
    ///
    /// One slot per survivor whose submesh has a loaded material; a materialless or not-yet-resident
    /// submesh is skipped, matching the direct draw it replaces. The triage is a by-product of the
    /// same single pass, not a separable step: translucent and skinned survivors are emitted into
    /// the two output lists rather than traversed for a second time.
    ///
    /// @pre Runs before GatherSkinned and GatherTranslucent, which consume its output lists and
    ///      continue its slot cursor — the static range must stay contiguous from 0, because the
    ///      GPU cull arrays are indexed by it.
    /// @param input          The shared per-frame inputs.
    /// @param survivors      The camera-frustum survivors, in ascending candidate-id order.
    /// @param plan           Receives the static slots and their groups.
    /// @param slotCursor     The shared slot cursor, advanced once per laid-out slot.
    /// @param skinnedOut     Receives the skinned survivors, in survivor order.
    /// @param translucentOut Receives the translucent survivors, in survivor order.
    void GatherStaticOpaque(const DrawGatherInput& input, std::span<const u32> survivors,
                            GBufferDrawPlan& plan, u32& slotCursor, vector<u32>& skinnedOut,
                            vector<u32>& translucentOut);

    /// @brief Lays out the skinned slots after the static range and writes their palettes.
    ///
    /// One palette per entity, shared by its submeshes and computed on first encounter from the
    /// entity's SkinnedPose or its bind pose. Each slot's DrawData carries the resulting
    /// PaletteBase; these draw on the CPU-direct skinned path.
    ///
    /// @pre GatherStaticOpaque ran, so the static range is already contiguous from 0.
    /// @param input                The shared per-frame inputs.
    /// @param skinned              The skinned survivors GatherStaticOpaque triaged out.
    /// @param plan                 Receives the skinned slots and their groups.
    /// @param paletteBaseByEntity  This frame's palette base per packed entity; read back by the
    ///                             shadow passes.
    /// @param slotCursor           The shared slot cursor, continued from the static range.
    /// @param paletteCursor        Matrices written into this frame's palette region so far.
    void GatherSkinned(const DrawGatherInput& input, std::span<const u32> skinned,
                       GBufferDrawPlan& plan, unordered_map<u64, u32>& paletteBaseByEntity,
                       u32& slotCursor, u32& paletteCursor);

    /// @brief Lays out the translucent draws after the opaque slots and sorts them for blending.
    ///
    /// Each draw reads its record from DrawData by the candidate id, exactly like a static surface
    /// draw; the translucent pass binds each material's own alpha-blended pipeline. The forward
    /// pass draws through the canonical (static) vertex layout, so a skinned mesh carrying a
    /// translucent material is not gathered here (opaque skinning, which uses the skinned vertex
    /// path, is unaffected).
    ///
    /// @pre GatherStaticOpaque and GatherSkinned ran, so their slot ranges are already laid out.
    /// @param input       The shared per-frame inputs.
    /// @param translucent The translucent survivors GatherStaticOpaque triaged out.
    /// @param plan        Receives the sorted translucent draws.
    /// @param slotCursor  The shared slot cursor, continued from the skinned range.
    void GatherTranslucent(const DrawGatherInput& input, std::span<const u32> translucent,
                           TranslucentDrawPlan& plan, u32& slotCursor);
}
