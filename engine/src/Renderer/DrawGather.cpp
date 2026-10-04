#include "DrawGather.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    bool CastsShadow(const std::span<const AssetHandle<MaterialInstance>> materials,
                     const Mesh& mesh, const u32 subMeshIndex)
    {
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
        mat4 ResolvePreviousWorld(const EntityFrameTable<mat4>& previousWorlds, const Entity owner,
                                  const mat4& currentWorld)
        {
            const mat4* previous = previousWorlds.Find(owner);
            return previous != nullptr ? *previous : currentWorld;
        }

        // The per-draw record every gathered draw writes: world, the normal matrix's three columns
        // (computed once per entity at gather), and the frame-folded material selector.
        GpuDrawData MakeDrawData(const VisibleMesh& item, const MaterialInstance& material,
                                 const mat4& prevWorld)
        {
            return GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(item.NormalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(item.NormalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(item.NormalMatrix[2], 0.0f),
                .MaterialOffset = material.GetMaterialSelector(),
                .EntityIndex = item.Owner.Index,
                .PrevWorld = prevWorld,
            };
        }

        // Whether two adjacent slots draw the same index range with consecutive candidate ids, so
        // one instanced draw covers both.
        bool ExtendsRun(const DrawSlot& previous, const DrawSlot& next)
        {
            return next.IndexCount == previous.IndexCount &&
                   next.FirstIndex == previous.FirstIndex &&
                   next.VertexOffset == previous.VertexOffset &&
                   next.CandidateId == previous.CandidateId + 1;
        }
    }

    void SortDrawKeys(const std::span<DrawKey> keys)
    {
        std::ranges::sort(keys,
                          [](const DrawKey& a, const DrawKey& b)
                          {
                              if (a.Pipeline != b.Pipeline)
                              {
                                  return std::less<const void*>{}(a.Pipeline, b.Pipeline);
                              }
                              if (a.SourceMesh != b.SourceMesh)
                              {
                                  return std::less<const Mesh*>{}(a.SourceMesh, b.SourceMesh);
                              }
                              if (a.SubMeshIndex != b.SubMeshIndex)
                              {
                                  return a.SubMeshIndex < b.SubMeshIndex;
                              }
                              return a.Candidate < b.Candidate;
                          });
    }

    void GroupContiguousSlots(const std::span<const DrawSlot> slots, vector<DrawGroup>& groups,
                              vector<InstanceRun>& runs)
    {
        for (u32 s = 0; s < slots.size();)
        {
            const Mesh* mesh = slots[s].SourceMesh;
            const void* pipeline = slots[s].PipelineKey;
            u32 count = 0;
            while (s + count < slots.size() && slots[s + count].SourceMesh == mesh &&
                   slots[s + count].PipelineKey == pipeline)
            {
                ++count;
            }

            const u32 firstRun = static_cast<u32>(runs.size());
            for (u32 r = s; r < s + count;)
            {
                u32 length = 1;
                while (r + length < s + count &&
                       ExtendsRun(slots[r + length - 1], slots[r + length]))
                {
                    ++length;
                }
                runs.push_back(InstanceRun{.FirstSlot = r, .Count = length});
                r += length;
            }

            groups.push_back(DrawGroup{.SourceMesh = mesh,
                                       .PipelineMaterial = slots[s].Pipeline,
                                       .FirstSlot = s,
                                       .SlotCount = count,
                                       .FirstRun = firstRun,
                                       .RunCount = static_cast<u32>(runs.size()) - firstRun});
            s += count;
        }
    }

    void GatherStaticOpaque(const DrawGatherInput& input, const std::span<const u32> survivors,
                            GBufferDrawPlan& plan, DrawBudget& budget, vector<u32>& skinnedOut,
                            vector<u32>& translucentOut, vector<DrawKey>& keyScratch)
    {
        // Triage every survivor first, keying the static ones; slots are claimed only once the
        // keys are ordered, so equal draws land in adjacent slots.
        keyScratch.clear();
        for (const u32 id : survivors)
        {
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const std::span<const AssetHandle<MaterialInstance>> materials = item.Materials;
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
            const MaterialInstance* material = materials[subMesh.MaterialIndex].Get();
            if (material->GetDomain() == MaterialDomain::Translucent)
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

            keyScratch.push_back(DrawKey{
                .Pipeline = material->GetParent().Get(),
                .SourceMesh = &mesh,
                .SubMeshIndex = candidate.SubMeshIndex,
                .Candidate = id,
            });
        }

        SortDrawKeys(keyScratch);

        for (usize index = 0; index < keyScratch.size(); ++index)
        {
            const DrawKey& key = keyScratch[index];
            u32 slot = 0;
            if (!budget.TryClaimSlot(slot))
            {
                budget.RecordDropped(DrawPhase::StaticOpaque,
                                     static_cast<u32>(keyScratch.size() - index));
                break;
            }

            const SubMeshCandidate& candidate = input.Candidates[key.Candidate];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const SubMesh& subMesh = key.SourceMesh->GetSubMeshes()[key.SubMeshIndex];
            const MaterialInstance* material = item.Materials[subMesh.MaterialIndex].Get();
            if (!plan.PipelineMaterial)
            {
                plan.PipelineMaterial = material;
            }

            input.DrawData[input.FrameBase + slot] =
                MakeDrawData(item, *material,
                             ResolvePreviousWorld(input.PreviousWorlds, item.Owner, item.World));

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
                .SourceMesh = key.SourceMesh,
                .Pipeline = material,
                .PipelineKey = key.Pipeline,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        GroupContiguousSlots(plan.Slots, plan.Groups, plan.Runs);
    }

    void GatherSkinned(const DrawGatherInput& input, const std::span<const u32> skinned,
                       GBufferDrawPlan& plan, EntityFrameTable<u32>& paletteBaseByEntity,
                       DrawBudget& budget)
    {
        for (usize index = 0; index < skinned.size(); ++index)
        {
            const u32 id = skinned[index];
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];
            const std::span<const AssetHandle<MaterialInstance>> materials = item.Materials;
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
            u32 paletteBase = 0;
            u32 slot = 0;
            const u32* existing = paletteBaseByEntity.Find(item.Owner);
            if (existing != nullptr)
            {
                paletteBase = *existing;
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

                paletteBaseByEntity.Set(item.Owner, paletteBase);
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();
            if (plan.SkinnedPipelineMaterial == nullptr)
            {
                plan.SkinnedPipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Velocity needs the previous frame's world and palette base for this entity (its
            // deformation motion). The previous palette data is still resident in its own ring
            // region. First seen → no motion (current values).
            const mat4 prevWorld =
                ResolvePreviousWorld(input.PreviousWorlds, item.Owner, item.World);
            const u32* prevBase = input.PreviousPaletteBases.Find(item.Owner);
            const u32 prevPaletteBase = prevBase != nullptr ? *prevBase : paletteBase;

            GpuDrawData drawData = MakeDrawData(item, material, prevWorld);
            drawData.PaletteBase = paletteBase;
            drawData.PrevPaletteBase = prevPaletteBase;
            input.DrawData[input.FrameBase + slot] = drawData;

            plan.SkinnedSlots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .Pipeline = materials[subMesh.MaterialIndex].Get(),
                .PipelineKey = material.GetParent().Get(),
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        GroupContiguousSlots(plan.SkinnedSlots, plan.SkinnedGroups, plan.SkinnedRuns);
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
            const std::span<const AssetHandle<MaterialInstance>> materials = item.Materials;
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            u32 slot = 0;
            if (!budget.TryClaimSlot(slot))
            {
                budget.RecordDropped(DrawPhase::Translucent,
                                     static_cast<u32>(translucent.size() - index));
                break;
            }

            const MaterialInstance& material = *materials[subMesh.MaterialIndex].Get();

            input.DrawData[input.FrameBase + slot] = MakeDrawData(
                item, material, ResolvePreviousWorld(input.PreviousWorlds, item.Owner, item.World));

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
                .SortPriority = parent->GetSortPriority() + item.SortPriority,
            });
        }

        // Ascending priority groups, back-to-front (most negative view-space z first) within
        // each: a higher priority — the material's, plus its entity's MeshRenderer::SortPriority —
        // draws over every lower-priority draw regardless of depth. Each plan sorts on its own, since the layer composites as a
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
