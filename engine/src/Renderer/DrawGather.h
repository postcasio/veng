#pragma once

#include <span>

#include <Veng/Renderer/EntityFrameTable.h>
#include <Veng/Renderer/SceneView.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Visibility.h>
#include <Veng/Veng.h>

#include "DrawBudget.h"
#include "DrawPlan.h"
#include "GpuBlocks.h"

namespace Veng::Renderer
{
    /// @brief Whether a submesh contributes to a shadow map — i.e. whether it occludes light.
    ///
    /// A submesh casts when it has a resident material that is not Translucent. The domain test is
    /// the substantive one: a Translucent surface writes no opaque depth, is drawn after the
    /// lighting it would have to occlude, and is documented as never occluding another translucent
    /// — so casting a solid, fully-opaque shadow from it contradicts every other way the domain
    /// behaves, and reads as a pane of glass painting a black rectangle on the floor. An unassigned
    /// or not-yet-resident material casts nothing, because there is nothing yet to say it should.
    ///
    /// Alpha-cut and stained-glass casters are deliberately out of scope: both need the shadow pass
    /// to shade rather than to rasterize depth, which is a separate capability from this predicate.
    /// @param materials     The material list to resolve the submesh against — the caller's
    ///                      VisibleMesh::Materials, so a per-entity override is honoured.
    /// @param mesh          The mesh owning the submesh.
    /// @param subMeshIndex  Index of the submesh within that mesh.
    /// @return True when the submesh should be rasterized into a shadow map.
    [[nodiscard]] bool CastsShadow(std::span<const AssetHandle<MaterialInstance>> materials,
                                   const Mesh& mesh, u32 subMeshIndex);

    /// @brief The per-frame inputs the three draw-gather phases share.
    ///
    /// Plain data plus the mapped write targets, bundled so each phase takes one const reference
    /// rather than a dozen positional parameters. The genuinely mutable state — the draw budget,
    /// the plans, the palette-base map — stays an explicit by-reference parameter, so mutation is
    /// visible at every call site.
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
        /// @brief Mapped per-draw DrawData records the phases write through.
        GpuDrawData* DrawData = nullptr;
        /// @brief Mapped GPU-cull candidate records for this frame's region, or null under CPU culling.
        GpuCullCandidate* CullData = nullptr;
        /// @brief Mapped skinning palette matrices; a palette base indexes this absolutely.
        mat4* PaletteData = nullptr;
        /// @brief Previous frame's world matrix per entity, the object-velocity source.
        const EntityFrameTable<mat4>& PreviousWorlds;
        /// @brief Previous frame's palette base per entity, the deformation-velocity source.
        const EntityFrameTable<u32>& PreviousPaletteBases;
    };

    /// @brief The sort key a batched pass orders its draws by before laying them out.
    ///
    /// Equal keys up to Candidate draw the same index range of the same mesh through the same
    /// pipeline, so ordering by this key makes every such set adjacent — one pipeline bind per
    /// pipeline, one buffer bind per mesh, one instanced draw per submesh. Candidate is the
    /// tiebreak, keeping gather order within a run.
    struct DrawKey
    {
        /// @brief The pipeline the draw binds (a g-buffer draw's parent material), or null for a
        ///        pass that draws everything through one pipeline.
        const void* Pipeline = nullptr;
        /// @brief The mesh whose buffers the draw binds.
        const Mesh* SourceMesh = nullptr;
        /// @brief The submesh within SourceMesh.
        u32 SubMeshIndex = 0;
        /// @brief The survivor's candidate id; the tiebreak, and how the caller finds it again.
        u32 Candidate = 0;
    };

    /// @brief Orders draw keys by (pipeline, mesh, submesh, candidate), so equal draws are adjacent.
    ///
    /// The order is total and depends only on the keys, so the same survivors always lay out the
    /// same way within a process. Pipeline and mesh compare by address, so the order between two
    /// distinct meshes is arbitrary but fixed — which is all batching needs.
    /// @param keys The keys to sort in place.
    void SortDrawKeys(std::span<DrawKey> keys);

    /// @brief Groups contiguous slots sharing a source mesh and a pipeline, and splits each group
    ///        into instanced runs.
    ///
    /// The mesh's buffers and the material pipeline each bind once per group. Splitting on the
    /// pipeline key (not just the mesh) is what lets surface materials with different fragment
    /// shaders coexist — each group binds its own. Within a group, a run is a maximal stretch of
    /// slots drawing the same index range with consecutive candidate ids, which one instanced draw
    /// covers. Pure: a span of slots in, groups and runs appended out.
    /// @param slots  The slots to group, in submission order.
    /// @param groups Receives one group per contiguous (mesh, pipeline) stretch; appended to.
    /// @param runs   Receives each group's runs, in order; appended to. A group's FirstRun indexes
    ///               this list as it stands after the call.
    void GroupContiguousSlots(std::span<const DrawSlot> slots, vector<DrawGroup>& groups,
                              vector<InstanceRun>& runs);

    /// @brief Lays out the static opaque slots and triages the survivors the later phases gather.
    ///
    /// One slot per survivor whose submesh has a loaded material; a materialless or not-yet-resident
    /// submesh is skipped, matching the direct draw it replaces. The triage runs first, over every
    /// survivor: translucent and skinned survivors go to the two output lists, and the static ones
    /// are keyed and ordered by SortDrawKeys before any slot is claimed, so equal draws take
    /// adjacent slots and the plan's runs batch them.
    ///
    /// @pre Runs before GatherSkinned and GatherTranslucent, which consume its output lists and
    ///      continue its slot cursor — the static range must stay contiguous from 0, because the
    ///      GPU cull arrays are indexed by it.
    /// An exhausted slot budget ends the phase: every static survivor not yet seated is counted as a
    /// static drop. The triaged lists are complete either way; the later phases find the budget
    /// spent and count their own drops.
    /// @param input          The shared per-frame inputs.
    /// @param survivors      The camera-frustum survivors, in ascending candidate-id order.
    /// @param plan           Receives the static slots, their groups and their runs.
    /// @param budget         The shared draw budget, claimed once per laid-out slot.
    /// @param skinnedOut     Receives the skinned survivors, in survivor order.
    /// @param translucentOut Receives the translucent survivors, in survivor order.
    /// @param keyScratch     Reused storage for the static survivors' sort keys; cleared here.
    void GatherStaticOpaque(const DrawGatherInput& input, std::span<const u32> survivors,
                            GBufferDrawPlan& plan, DrawBudget& budget, vector<u32>& skinnedOut,
                            vector<u32>& translucentOut, vector<DrawKey>& keyScratch);

    /// @brief Lays out the skinned slots after the static range and writes their palettes.
    ///
    /// One palette per entity, shared by its submeshes and computed on first encounter from the
    /// entity's SkinnedPose or its bind pose. Each slot's DrawData carries the resulting
    /// PaletteBase; these draw on the CPU-direct skinned path.
    ///
    /// @pre GatherStaticOpaque ran, so the static range is already contiguous from 0.
    /// @param input                The shared per-frame inputs.
    /// @param skinned              The skinned survivors GatherStaticOpaque triaged out.
    /// @param plan                 Receives the skinned slots, their groups and their runs.
    /// @param paletteBaseByEntity  This frame's palette base per entity, begun by the caller; read
    ///                             back by the shadow passes.
    /// @param budget               The shared draw budget, continued from the static range; an
    ///                             entity's first submesh claims its slot and palette together.
    void GatherSkinned(const DrawGatherInput& input, std::span<const u32> skinned,
                       GBufferDrawPlan& plan, EntityFrameTable<u32>& paletteBaseByEntity,
                       DrawBudget& budget);

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
    /// @param plan        Receives the sorted full-resolution translucent draws.
    /// @param halfResPlan Receives the sorted draws of materials that opted into the
    ///                    reduced-resolution translucent layer (Material::IsHalfResolution).
    /// @param budget      The shared draw budget, continued from the skinned range.
    void GatherTranslucent(const DrawGatherInput& input, std::span<const u32> translucent,
                           TranslucentDrawPlan& plan, TranslucentDrawPlan& halfResPlan,
                           DrawBudget& budget);

    /// @brief Folds one translucent plan's draws into another, restoring the sort.
    ///
    /// The fallback for a frame that gathered half-resolution draws the wired passes cannot
    /// take yet — the layer's first active frame, or a frame whose view budget refused the
    /// layer's view slot: the draws render full-resolution this frame, in the correct
    /// back-to-front order, and @p from is left empty.
    /// @param into The plan that receives the draws (re-sorted after the append).
    /// @param from The plan whose draws move; cleared.
    void MergeTranslucentPlans(TranslucentDrawPlan& into, TranslucentDrawPlan& from);
}
