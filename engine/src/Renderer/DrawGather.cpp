#include "DrawGather.h"

#include <algorithm>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Assert.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
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
                            GBufferDrawPlan& plan, u32& slotCursor, vector<u32>& skinnedOut,
                            vector<u32>& translucentOut)
    {
        for (const u32 id : survivors)
        {
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

            const u32 slot = slotCursor;
            if (slot >= input.MaxSlots)
            {
                VE_ASSERT(false, "Draw gather: per-frame candidate count exceeds the maximum {}",
                          input.MaxSlots);
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
            ++slotCursor;
        }

        GroupContiguousSlots(plan.Slots, plan.Groups);
    }

    void GatherSkinned(const DrawGatherInput& input, const std::span<const u32> skinned,
                       GBufferDrawPlan& plan, unordered_map<u64, u32>& paletteBaseByEntity,
                       u32& slotCursor, u32& paletteCursor)
    {
        for (const u32 id : skinned)
        {
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
            const auto existing = paletteBaseByEntity.find(packed);
            if (existing != paletteBaseByEntity.end())
            {
                paletteBase = existing->second;
            }
            else
            {
                if (paletteCursor + boneCount > input.MaxPaletteMatrices)
                {
                    continue; // palette budget exhausted this frame
                }
                paletteBase = input.PaletteRegionBase + paletteCursor;

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

                paletteCursor += boneCount;
                paletteBaseByEntity[packed] = paletteBase;
            }

            const u32 slot = slotCursor;
            if (slot >= input.MaxSlots)
            {
                break;
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
            ++slotCursor;
        }

        GroupContiguousSlots(plan.SkinnedSlots, plan.SkinnedGroups);
    }

    void GatherTranslucent(const DrawGatherInput& input, const std::span<const u32> translucent,
                           TranslucentDrawPlan& plan, u32& slotCursor)
    {
        const mat4 viewMatrix = input.View.Camera.View();
        for (const u32 id : translucent)
        {
            const SubMeshCandidate& candidate = input.Candidates[id];
            const VisibleMesh& item = input.View.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            if (mesh.IsSkinned())
            {
                continue;
            }
            const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            const u32 slot = slotCursor;
            if (slot >= input.MaxSlots)
            {
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

            // Sort key: the submesh center in view space. The camera looks down -Z, so a farther
            // submesh has a more negative z; sorting ascending by z draws farthest first.
            const vec3 center = (item.WorldBounds.Min + item.WorldBounds.Max) * 0.5f;
            const f32 viewDepth = (viewMatrix * vec4(center, 1.0f)).z;

            plan.Draws.push_back(TranslucentDraw{
                .Material = &material,
                .SourceMesh = &mesh,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .CandidateId = slot,
                .ViewDepth = viewDepth,
                .SortPriority = material.GetParent().Get()->GetSortPriority(),
            });
            ++slotCursor;
        }

        // Ascending priority groups, back-to-front (most negative view-space z first) within
        // each: a higher-priority material (an overlay) draws over every lower-priority draw
        // regardless of depth.
        std::ranges::sort(plan.Draws,
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
