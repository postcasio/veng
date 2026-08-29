#include "DrawGather.h"

#include <algorithm>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    bool CastsShadow(const Mesh& mesh, const u32 subMeshIndex)
    {
        const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
        const SubMesh& subMesh = mesh.GetSubMeshes()[subMeshIndex];
        if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
            !materials[subMesh.MaterialIndex].IsLoaded())
        {
            return false;
        }
        return materials[subMesh.MaterialIndex].Get()->GetDomain() != MaterialDomain::Translucent;
    }

    namespace
    {
        // Last frame's matrix for this entity, or the current one (zero object motion) when first
        // seen — the previous world the surface pass writes velocity from.
        mat4 ResolvePreviousWorld(const unordered_map<u64, mat4>& previousWorlds, const u64 packed,
                                  const mat4& currentWorld)
        {
            const auto it = previousWorlds.find(packed);
            if (it != previousWorlds.end())
            {
                return it->second;
            }
            return currentWorld;
        }
    }

    void GroupContiguousSlots(const std::span<const DrawSlot> slots, vector<DrawGroup>& groups)
    {
        for (u32 s = 0; s < slots.size();)
        {
            const Mesh* mesh = slots[s].SourceMesh;
            const MaterialInstance* pipeline = slots[s].Pipeline;
            u32 count = 0;
            while (s + count < slots.size() && slots[s + count].SourceMesh == mesh &&
                   slots[s + count].Pipeline == pipeline)
            {
                ++count;
            }
            groups.push_back(DrawGroup{.SourceMesh = mesh,
                                       .PipelineMaterial = pipeline,
                                       .FirstSlot = s,
                                       .SlotCount = count});
            s += count;
        }
    }

    void GatherStaticOpaque(const DrawGatherInput& input, const std::span<const u32> survivors,
                            GBufferDrawPlan& plan, DrawBudget& budget, vector<u32>& skinnedOut,
                            vector<u32>& translucentOut)
    {
        for (usize index = 0; index < survivors.size(); ++index)
        {
            const u32 id = survivors[index];
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                !materials[subMesh.MaterialIndex].IsLoaded())
            {
                continue;
            }

            // Translucent submeshes are excluded from the g-buffer/opaque draw list and collected
            // into the forward translucent plan (they output final color through the forward pass,
            // not the g-buffer). The frustum-survivor set is shared — a translucent submesh is
            // simply routed to a different draw list.
            if (materials[subMesh.MaterialIndex].Get()->GetDomain() == MaterialDomain::Translucent)
            {
                translucentOut.push_back(id);
                continue;
            }

            // Skinned survivors are deferred to a second pass (they draw on the CPU-direct skinned
            // path after the static slots, which must stay contiguous from 0 for the GPU cull
            // arrays).
            if (mesh.IsSkinned())
            {
                skinnedOut.push_back(id);
                continue;
            }

            // Exhausting the budget ends the triage above too, so every remaining survivor is
            // lost — including ones the skinned and translucent phases would have gathered.
            u32 slot = 0;
            if (!budget.TryClaimSlot(slot))
            {
                budget.RecordDropped(DrawPhase::StaticOpaque,
                                     static_cast<u32>(survivors.size() - index));
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();
            if (!plan.PipelineMaterial)
            {
                plan.PipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Per-draw record: world matrix, the normal matrix's three columns (inverse-
            // transpose of the upper 3×3, correct under non-uniform scale), and the
            // frame-folded material selector.
            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            const mat4 prevWorld =
                ResolvePreviousWorld(input.PreviousWorlds, PackEntity(item.Owner), item.World);
            input.DrawData[input.FrameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            if (input.CullData != nullptr)
            {
                const AABB& bounds = item.WorldBounds;
                input.CullData[slot] = GpuCullCandidate{
                    .BoundsMin = vec4(bounds.Min, 0.0f),
                    .BoundsMax = vec4(bounds.Max, 0.0f),
                    .IndexCount = subMesh.IndexCount,
                    .FirstIndex = subMesh.IndexOffset,
                    .VertexOffset = 0,
                    .FirstInstance = slot,
                };
            }

            plan.Slots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .Pipeline = materials[subMesh.MaterialIndex].Get(),
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        GroupContiguousSlots(plan.Slots, plan.Groups);
    }

    void GatherSkinned(const DrawGatherInput& input, const std::span<const u32> skinned,
                       GBufferDrawPlan& plan, unordered_map<u64, u32>& paletteBaseByEntity,
                       DrawBudget& budget)
    {
        for (usize index = 0; index < skinned.size(); ++index)
        {
            const u32 id = skinned[index];
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const AssetHandle<Skeleton>& skeletonHandle = mesh.GetSkeleton();
            if (!skeletonHandle.IsLoaded())
            {
                continue;
            }
            const Skeleton& skeleton = *skeletonHandle.Get();
            const u32 boneCount = static_cast<u32>(skeleton.GetBoneCount());

            // One palette per entity, shared by its submeshes. Computed on first encounter from
            // the entity's SkinnedPose (the animation system's output) or the bind pose when the
            // entity has none (e.g. the editor with systems paused).
            const u64 packed = PackEntity(item.Owner);
            u32 paletteBase = 0;
            u32 slot = 0;
            const auto existing = paletteBaseByEntity.find(packed);
            if (existing != paletteBaseByEntity.end())
            {
                paletteBase = existing->second;
                if (!budget.TryClaimSlot(slot))
                {
                    budget.RecordDropped(DrawPhase::Skinned,
                                         static_cast<u32>(skinned.size() - index));
                    break;
                }
            }
            else
            {
                // One claim for the slot and the palette together: a half-committed claim would
                // either burn a slot no DrawSlot is written for, or leave a live palette base for
                // a draw that never happens (the shadow passes and next frame's velocity read it).
                u32 relativeBase = 0;
                const SkinnedClaim claim =
                    budget.TryClaimSkinnedDraw(boneCount, slot, relativeBase);
                if (claim == SkinnedClaim::PaletteExhausted)
                {
                    continue;
                }
                if (claim == SkinnedClaim::SlotsExhausted)
                {
                    budget.RecordDropped(DrawPhase::Skinned,
                                         static_cast<u32>(skinned.size() - index));
                    break;
                }
                paletteBase = input.PaletteRegionBase + relativeBase;

                const auto* pose = input.View.World.TryGet<SkinnedPose>(item.Owner);
                if (pose != nullptr && pose->Skinning.size() == boneCount)
                {
                    std::memcpy(input.PaletteData + paletteBase, pose->Skinning.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }
                else
                {
                    vector<mat4> bind;
                    skeleton.ComputeBindPoseMatrices(bind);
                    std::memcpy(input.PaletteData + paletteBase, bind.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }

                paletteBaseByEntity[packed] = paletteBase;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();
            if (plan.SkinnedPipelineMaterial == nullptr)
            {
                plan.SkinnedPipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Velocity needs the previous frame's world and palette base for this entity (its
            // deformation motion). The previous palette data is still resident in its own ring
            // region. First seen → no motion (current values).
            const mat4 prevWorld = ResolvePreviousWorld(input.PreviousWorlds, packed, item.World);
            u32 prevPaletteBase = paletteBase;
            {
                const auto prevBaseIt = input.PreviousPaletteBases.find(packed);
                if (prevBaseIt != input.PreviousPaletteBases.end())
                {
                    prevPaletteBase = prevBaseIt->second;
                }
            }

            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            input.DrawData[input.FrameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .PaletteBase = paletteBase,
                .PrevPaletteBase = prevPaletteBase,
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            plan.SkinnedSlots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .Pipeline = materials[subMesh.MaterialIndex].Get(),
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        GroupContiguousSlots(plan.SkinnedSlots, plan.SkinnedGroups);
    }

    void GatherTranslucent(const DrawGatherInput& input, const std::span<const u32> translucent,
                           TranslucentDrawPlan& plan, TranslucentDrawPlan& halfResPlan,
                           DrawBudget& budget)
    {
        const mat4 viewMatrix = input.View.Camera.View();
        for (usize index = 0; index < translucent.size(); ++index)
        {
            const u32 id = translucent[index];
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            if (mesh.IsSkinned())
            {
                continue;
            }
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            u32 slot = 0;
            if (!budget.TryClaimSlot(slot))
            {
                budget.RecordDropped(DrawPhase::Translucent,
                                     static_cast<u32>(translucent.size() - index));
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();

            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            const mat4 prevWorld =
                ResolvePreviousWorld(input.PreviousWorlds, PackEntity(item.Owner), item.World);
            input.DrawData[input.FrameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };

            // Sort key: the submesh's *own* center in view space. The camera looks down -Z, so a
            // farther submesh has a more negative z; sorting ascending by z draws farthest first.
            // The mesh's whole bound gives every submesh of one mesh the same key, which leaves
            // their relative order arbitrary — so a mesh partitioned into submeshes precisely to be
            // ordered against itself, or against something concentric with it, sorts as though it
            // had not been. A submesh's bound is folded at load, so this is a matrix-vector product
            // per draw. A submesh whose range referenced no vertices has an empty bound and falls
            // back to the mesh's.
            const vec3 center = subMesh.Bounds.IsEmpty()
                                    ? (item.WorldBounds.Min + item.WorldBounds.Max) * 0.5f
                                    : vec3(item.World * vec4(subMesh.Bounds.Center(), 1.0f));
            const f32 viewDepth = (viewMatrix * vec4(center, 1.0f)).z;

            // A material that opted into the reduced-resolution layer routes to its own plan;
            // the layer composites under every full-resolution translucent, so the split is a
            // sort statement as much as a cost one.
            const Material* parent = material.GetParent().Get();
            TranslucentDrawPlan& destination = parent->IsHalfResolution() ? halfResPlan : plan;
            destination.Draws.push_back(TranslucentDraw{
                .Material = &material,
                .SourceMesh = &mesh,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .CandidateId = slot,
                .ViewDepth = viewDepth,
                .SortPriority = parent->GetSortPriority(),
            });
        }

        // Ascending priority groups, back-to-front (most negative view-space z first) within
        // each: a higher-priority material (an overlay) draws over every lower-priority draw
        // regardless of depth. Each plan sorts on its own, since the layer composites as a
        // whole under the full-resolution draws.
        const auto backToFront = [](const TranslucentDraw& a, const TranslucentDraw& b)
        {
            if (a.SortPriority != b.SortPriority)
            {
                return a.SortPriority < b.SortPriority;
            }
            return a.ViewDepth < b.ViewDepth;
        };
        std::ranges::sort(plan.Draws, backToFront);
        std::ranges::sort(halfResPlan.Draws, backToFront);
    }

    void MergeTranslucentPlans(TranslucentDrawPlan& into, TranslucentDrawPlan& from)
    {
        into.Draws.insert(into.Draws.end(), from.Draws.begin(), from.Draws.end());
        from.Draws.clear();
        std::ranges::sort(into.Draws,
                          [](const TranslucentDraw& a, const TranslucentDraw& b)
                          {
                              if (a.SortPriority != b.SortPriority)
                              {
                                  return a.SortPriority < b.SortPriority;
                              }
                              return a.ViewDepth < b.ViewDepth;
                          });
    }
}
