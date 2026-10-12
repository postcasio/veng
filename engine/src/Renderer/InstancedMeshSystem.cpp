#include "InstancedMeshSystem.h"

#include "DrawBudget.h"
#include "GpuBlocks.h"
#include "InstancedCull.h"

#include <algorithm>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The per-instance cull compute shader and the depth-only set vertex stage.
        constexpr AssetId InstancedCullCompId{0x8339B1E0501989BDULL};
        constexpr AssetId ShadowDepthInstancedVertId{0x568733D823738867ULL};

        // Shadow depth targets: the cascade and punctual atlases are both D32.
        constexpr Format ShadowFormat = Format::D32Sfloat;

        // Invocations per cull workgroup, matching instanced_mesh_cull.comp's numthreads.
        constexpr u32 CullGroupSize = 64;
        // The most workgroups one dispatch dimension takes (the spec's guaranteed minimum).
        constexpr u32 MaxGroupsPerDimension = 65535;

        // A record is 16 words; a list entry is one.
        constexpr u32 RecordWords = sizeof(MeshInstance) / sizeof(u32);

        // The view flags, matching instanced_mesh_cull.comp.
        constexpr u32 ViewMain = 1u;
        constexpr u32 ViewFrustumCull = 2u;
        constexpr u32 ViewPerspective = 4u;

        // instanced_mesh_cull.comp's CullFrame, at the start of the table buffer.
        struct GpuCullFrame
        {
            mat4 ReferenceViewProj;
            mat4 PrevViewProj;
            vec4 ReferenceEye;
            uvec2 HiZExtent;
            u32 HiZTexture;
            u32 OcclusionValid;
            f32 DepthBias;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };
        static_assert(sizeof(GpuCullFrame) == 176, "GpuCullFrame must match CullFrame");

        // instanced_mesh_cull.comp's CullJob.
        struct GpuCullJob
        {
            u32 ThreadBase;
            u32 Set;
            u32 View;
            u32 CommandBase;
            u32 ListBase;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };
        static_assert(sizeof(GpuCullJob) == 32, "GpuCullJob must match CullJob");

        // instanced_mesh_cull.comp's CullSet.
        struct GpuCullSet
        {
            mat4 World;
            vec4 BoundsMin;
            vec4 BoundsMax;
            vec4 LodLow;
            vec4 LodHigh;
            u32 RecordBase;
            u32 Count;
            u32 LodCount;
            u32 SubMeshCount;
            f32 MinPixels;
            f32 MaxPixels;
            f32 MinShadowTexels;
            i32 ForcedLod;
            u32 LodMemoryBase;
            u32 HistoryValid;
            u32 Pad0;
            u32 Pad1;
        };
        static_assert(sizeof(GpuCullSet) == 176, "GpuCullSet must match CullSet");

        // instanced_mesh_cull.comp's CullView.
        struct GpuCullView
        {
            vec4 Planes[6];
            vec4 Eye;
            f32 LodScreenScale;
            f32 Hysteresis;
            u32 Flags;
            f32 Resolution;
            i32 ForcedLod;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };
        static_assert(sizeof(GpuCullView) == 144, "GpuCullView must match CullView");

        // instanced_mesh_cull.comp's push block.
        struct CullPush
        {
            u32 JobsOffset;
            u32 SetsOffset;
            u32 ViewsOffset;
            u32 JobCount;
            u32 ThreadCount;
            u32 RowThreads;
        };

        // shadow_depth_instanced.vert's push block.
        struct ShadowInstancedPush
        {
            mat4 ViewProjWorld;
            u32 InstanceBuffer;
            u32 InstanceBase;
        };

        // A buffer grows by this much past what a frame asked for, so a slowly growing population
        // does not reallocate every frame.
        u64 GrownSize(const u64 wanted, const u64 floor)
        {
            return std::max(floor, wanted + wanted / 4);
        }

        // The cull's view of one view: its planes, level view and flags.
        GpuCullView MakeView(const Frustum& frustum, const LodView& lod, const f32 hysteresis,
                             const u32 flags, const f32 resolution, const i32 forced)
        {
            GpuCullView view{};
            for (u32 p = 0; p < 6; ++p)
            {
                view.Planes[p] = frustum.Planes[p];
            }
            view.Eye = vec4(lod.Eye, lod.ProjectionScale);
            view.LodScreenScale = lod.ScreenScale;
            view.Hysteresis = hysteresis;
            view.Flags = flags | (lod.Perspective ? ViewPerspective : 0u);
            view.Resolution = resolution;
            view.ForcedLod = forced;
            return view;
        }
    }

    Unique<InstancedMeshSystem> InstancedMeshSystem::Create(Context& context, AssetManager& assets)
    {
        return Unique<InstancedMeshSystem>(new InstancedMeshSystem(context, assets));
    }

    InstancedMeshSystem::InstancedMeshSystem(Context& context, AssetManager& assets)
        : m_Context(context), m_Assets(assets)
    {
        const AssetResult<AssetHandle<Veng::Shader>> cullCs =
            assets.LoadSync<Veng::Shader>(InstancedCullCompId);
        VE_ASSERT(cullCs.has_value(), "InstancedMeshSystem: cull shader load failed: {}",
                  cullCs.has_value() ? "" : cullCs.error().Detail);

        const auto StorageBinding = [](const u32 binding)
        {
            return DescriptorBinding{.Binding = binding,
                                     .Type = DescriptorType::StorageBuffer,
                                     .Count = 1,
                                     .Stages = ShaderStage::Compute};
        };
        m_CullSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Instanced Cull Set Layout",
                           .Bindings = {StorageBinding(0), StorageBinding(1), StorageBinding(2),
                                        StorageBinding(3)},
                       });
        m_CullLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer Instanced Cull Layout",
                .DescriptorSetLayouts = {m_CullSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<CullPush>(ShaderStage::Compute)},
            });
        m_CullPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Instanced Cull Pipeline",
                .PipelineLayout = m_CullLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullCs->Get()->Module},
            });

        m_ShadowLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Instanced Shadow Layout",
                           .PushConstantRanges = {PushConstantRange::Of<ShadowInstancedPush>(
                               ShaderStage::Vertex)},
                       });
        m_ShadowPipeline = BuildShadowPipeline(assets, false);
        m_ShadowClampPipeline = m_Context.IsDepthClampSupported()
                                    ? BuildShadowPipeline(assets, true)
                                    : m_ShadowPipeline;

        m_Slots.resize(m_Context.GetMaxFramesInFlight());
    }

    InstancedMeshSystem::~InstancedMeshSystem()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        for (const FrameSlot& slot : m_Slots)
        {
            bindless.Release(slot.DataHandle);
        }
    }

    Ref<GraphicsPipeline> InstancedMeshSystem::BuildShadowPipeline(AssetManager& assets,
                                                                   const bool depthClamp)
    {
        const AssetResult<AssetHandle<Veng::Shader>> vs =
            assets.LoadSync<Veng::Shader>(ShadowDepthInstancedVertId);
        VE_ASSERT(vs.has_value(), "InstancedMeshSystem: shadow vertex shader load failed: {}",
                  vs.has_value() ? "" : vs.error().Detail);

        optional<VertexBufferLayout> vertexBufferLayout;
        const ShaderInterface& vsInterface = vs->Get()->Interface;
        if (vsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layout =
                assets.LoadSync<Veng::VertexLayout>(*vsInterface.VertexLayoutId);
            VE_ASSERT(layout.has_value(), "InstancedMeshSystem: vertex layout load failed: {}",
                      layout.has_value() ? "" : layout.error().Detail);
            vertexBufferLayout = layout->Get()->GetLayout();
        }

        // The shadow passes' own state — front-face culling, depth write, and the cascades' depth
        // clamp — so a set's shadow matches an entity caster's.
        return GraphicsPipeline::Create(
            m_Context,
            {
                .Name = depthClamp ? "SceneRenderer Instanced Shadow Clamp Pipeline"
                                   : "SceneRenderer Instanced Shadow Pipeline",
                .ColorAttachments = {},
                .DepthAttachmentFormat = ShadowFormat,
                .VertexBufferLayout = vertexBufferLayout,
                .PipelineLayout = m_ShadowLayout,
                .ShaderStages = {{.Stage = ShaderStage::Vertex, .Module = vs->Get()->Module}},
                .CullMode = CullMode::Front,
                .DepthTestEnable = true,
                .DepthWriteEnable = true,
                .DepthClampEnable = depthClamp,
            });
    }

    void InstancedMeshSystem::Gather(const Scene& scene, const Entity exclude, const f32 alpha)
    {
        VE_PROFILE_SCOPE("Render/InstancedGather");
        m_Sets.clear();
        m_Bounds = AABB::Empty();
        m_CasterBounds = AABB::Empty();
        if (scene.TryGetFirst<InstancedMesh>() == nullptr)
        {
            m_PreviousWorlds.clear();
            m_RecordCount = 0;
            return;
        }

        scene.UpdateWorldTransforms();
        const bool interpolate = alpha != 0.0f && scene.HasTransformInterpolation();
        if (interpolate)
        {
            scene.UpdateInterpolatedWorldTransforms(alpha);
        }

        const auto Report = [this](const Entity entity, const string& why)
        {
            if (std::ranges::find(m_Reported, entity) != m_Reported.end())
            {
                return;
            }
            m_Reported.push_back(entity);
            Log::Error("InstancedMesh on entity {}: {}; the set is not drawn.", entity.Index, why);
        };

        for (auto [entity, set] : scene.View<InstancedMesh>())
        {
            if (entity == exclude || !set.Visible || set.Instances == nullptr ||
                set.Instances->GetCount() == 0 || !scene.Has<Transform>(entity) ||
                !set.Mesh.IsLoaded() || !set.Material.IsLoaded())
            {
                continue;
            }
            const Mesh& mesh = *set.Mesh.Get();
            if (mesh.IsSkinned())
            {
                Report(entity, "its mesh is skinned, and a set draws static meshes only");
                continue;
            }
            const MaterialInstance& material = *set.Material.Get();
            const Material& parent = *material.GetParent().Get();
            if (parent.GetDomain() != MaterialDomain::Surface || !parent.SupportsInstancedSets())
            {
                Report(entity, fmt::format("its material '{}' does not declare \"instancedSets\"",
                                           parent.GetName()));
                continue;
            }

            const mat4 world = interpolate ? scene.GetInterpolatedWorldTransform(entity, alpha)
                                           : WorldMatrix(scene, entity);
            const auto previous =
                std::ranges::find(m_PreviousWorlds, entity, &std::pair<Entity, mat4>::first);
            const auto* lodOverride = scene.TryGet<LodOverride>(entity);
            const SetEntry entry{
                .Owner = entity,
                .World = world,
                .PrevWorld = previous != m_PreviousWorlds.end() ? previous->second : world,
                .WorldBounds = set.Instances->GetLocalBounds(mesh.GetBounds()).Transformed(world),
                .SourceMesh = &mesh,
                .Material = &material,
                .Store = set.Instances.get(),
                .Count = set.Instances->GetCount(),
                .CastsShadows = set.CastsShadows,
                .MinPixels = set.MinProjectedPixels,
                .MaxPixels = set.MaxProjectedPixels,
                .MinShadowTexels = set.MinShadowCasterTexels,
                .ForcedLod = lodOverride != nullptr ? lodOverride->Level : AutomaticLod,
            };
            m_Bounds = Union(m_Bounds, entry.WorldBounds);
            if (entry.CastsShadows)
            {
                m_CasterBounds = Union(m_CasterBounds, entry.WorldBounds);
            }
            m_Sets.push_back(entry);
        }

        m_PreviousWorlds.clear();
        for (const SetEntry& entry : m_Sets)
        {
            m_PreviousWorlds.emplace_back(entry.Owner, entry.World);
        }

        // The pooled layout: each set's whole capacity, in gather order. A changed key moves every
        // set's records, so every frame slot re-uploads them and the level memory starts over.
        vector<std::pair<u64, u64>> key;
        key.reserve(m_Sets.size());
        u32 recordBase = 0;
        for (SetEntry& entry : m_Sets)
        {
            key.emplace_back(entry.Store->GetId(), entry.Store->GetLayoutSerial());
            entry.RecordBase = recordBase;
            recordBase += entry.Store->GetCapacity();
        }
        if (key != m_LayoutKey)
        {
            m_LayoutKey = std::move(key);
            ++m_LayoutSerial;
            m_LodHistoryValid = false;
            m_PreviousMainSets.clear();
        }
        m_RecordCount = recordBase;
    }

    u32 InstancedMeshSystem::ClaimDrawSlots(GpuDrawData* drawData, DrawBudget& budget)
    {
        u32 instances = 0;
        for (usize k = 0; k < m_Sets.size(); ++k)
        {
            SetEntry& entry = m_Sets[k];
            u32 slot = 0;
            if (!budget.TryClaimSlot(slot))
            {
                budget.RecordDropped(DrawPhase::InstancedSet, static_cast<u32>(m_Sets.size() - k));
                break;
            }
            const mat3 normal = glm::inverseTranspose(mat3(entry.World));
            drawData[slot] = GpuDrawData{
                .World = entry.World,
                .NormalColumn0 = vec4(normal[0], 0.0f),
                .NormalColumn1 = vec4(normal[1], 0.0f),
                .NormalColumn2 = vec4(normal[2], 0.0f),
                .MaterialOffset = entry.Material->GetMaterialSelector(),
                .EntityIndex = entry.Owner.Index,
                .PrevWorld = entry.PrevWorld,
            };
            entry.DrawSlot = slot;
            entry.Seated = true;
            entry.Material->EnsureInstancedPipeline(m_Assets);
            instances += entry.Count;
        }
        return instances;
    }

    void InstancedMeshSystem::EnsureCapacity(FrameSlot& slot, const u64 dataBytes,
                                             const u64 commandBytes, const u64 tableBytes)
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bool rebind = slot.BoundLod != m_LodMemory.get();
        if (slot.Data == nullptr || slot.Data->GetSize() < dataBytes)
        {
            bindless.Release(slot.DataHandle);
            slot.Data = Buffer::Create(m_Context, {
                                                      .Name = "SceneRenderer Instanced Data",
                                                      .Size = GrownSize(dataBytes, 64 * 1024),
                                                      .Usage = BufferUsage::Storage,
                                                      .HostMapped = true,
                                                  });
            slot.DataHandle = bindless.Register(slot.Data);
            slot.SyncedLayout = 0;
            rebind = true;
        }
        if (slot.Commands == nullptr || slot.Commands->GetSize() < commandBytes)
        {
            slot.Commands =
                Buffer::Create(m_Context, {
                                              .Name = "SceneRenderer Instanced Commands",
                                              .Size = GrownSize(commandBytes, 4 * 1024),
                                              .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                              .HostMapped = true,
                                          });
            rebind = true;
        }
        if (slot.Tables == nullptr || slot.Tables->GetSize() < tableBytes)
        {
            slot.Tables = Buffer::Create(m_Context, {
                                                        .Name = "SceneRenderer Instanced Tables",
                                                        .Size = GrownSize(tableBytes, 4 * 1024),
                                                        .Usage = BufferUsage::Storage,
                                                        .HostMapped = true,
                                                    });
            rebind = true;
        }
        if (rebind)
        {
            // A frame still in flight may hold the old set bound, so a fresh one is written and
            // the old retires with the frame that used it.
            slot.CullSet = DescriptorSet::Create(
                m_Context, {.Name = "SceneRenderer Instanced Cull Set", .Layout = m_CullSetLayout});
            slot.CullSet->Write(0, slot.Data);
            slot.CullSet->Write(1, slot.Commands);
            slot.CullSet->Write(2, m_LodMemory);
            slot.CullSet->Write(3, slot.Tables);
            slot.BoundLod = m_LodMemory.get();
        }
    }

    void InstancedMeshSystem::SyncRecords(FrameSlot& slot)
    {
        auto* records = static_cast<MeshInstance*>(slot.Data->GetMappedData());
        const bool whole = slot.SyncedLayout != m_LayoutSerial;
        slot.SyncedWrites.resize(m_Sets.size(), 0);
        for (usize k = 0; k < m_Sets.size(); ++k)
        {
            const SetEntry& entry = m_Sets[k];
            const std::span<const MeshInstance> instances = entry.Store->GetInstances();
            const InstanceSet::Range written =
                whole ? InstanceSet::Range{.First = 0, .End = static_cast<u32>(instances.size())}
                      : entry.Store->GetWrittenSince(slot.SyncedWrites[k]);
            if (written.End > written.First)
            {
                std::memcpy(records + entry.RecordBase + written.First,
                            instances.data() + written.First,
                            static_cast<usize>(written.End - written.First) * sizeof(MeshInstance));
            }
            slot.SyncedWrites[k] = entry.Store->GetWriteSerial();
        }
        slot.SyncedLayout = m_LayoutSerial;
    }

    void InstancedMeshSystem::PrepareFrame(const InstancedFrameViews& views)
    {
        VE_PROFILE_SCOPE("Render/InstancedPrepare");
        m_FrameIndex = views.FrameIndex;
        FrameSlot& slot = m_Slots[views.FrameIndex];
        m_GpuCulled = m_Context.IsGpuDrivenCullingSupported() && !m_ForceCpuCull;

        // The views: the main camera, then each shadow view in render order.
        const Frustum cameraFrustum = Frustum::FromViewProjection(views.CameraViewProj);
        const u32 cullFlag = views.FrustumCull ? ViewFrustumCull : 0u;
        vector<GpuCullView> viewRecords;
        vector<Frustum> frusta;
        vector<InstancedCullView> hostViews;
        viewRecords.push_back(MakeView(cameraFrustum, views.CameraLod, views.Hysteresis,
                                       ViewMain | cullFlag, 0.0f, views.ForcedLod));
        frusta.push_back(cameraFrustum);
        hostViews.push_back(InstancedCullView{
            .Planes = cameraFrustum,
            .FrustumCull = views.FrustumCull,
            .Main = true,
            .Lod = views.CameraLod,
            .Hysteresis = views.Hysteresis,
            .ForcedLod = views.ForcedLod,
        });
        if (views.Shadows != nullptr)
        {
            const ShadowLodSettings& lod = views.Shadows->GetLodSettings();
            for (const ShadowViewInfo& info : views.Shadows->GetViews())
            {
                LodView lodView = LodView::FromViewProjection(info.LodViewProj, lod.Bias);
                lodView.ScreenScale *= lod.ScreenScale;
                const Frustum frustum = Frustum::FromViewProjection(info.CullViewProj);
                viewRecords.push_back(MakeView(frustum, lodView, 0.0f, cullFlag,
                                               static_cast<f32>(info.Resolution), lod.Forced));
                frusta.push_back(frustum);
                hostViews.push_back(InstancedCullView{
                    .Planes = frustum,
                    .FrustumCull = views.FrustumCull,
                    .Main = false,
                    .Lod = lodView,
                    .Resolution = static_cast<f32>(info.Resolution),
                    .ForcedLod = lod.Forced,
                });
            }
        }

        // The jobs: every (view, set) pair the view does not reject whole.
        m_Jobs.clear();
        m_ViewJobs.assign(viewRecords.size(), {0u, 0u});
        m_ThreadCount = 0;
        u32 commandCount = 0;
        const u32 listWordBase = m_RecordCount * RecordWords;
        u32 listWords = 0;
        u32 draws = 0;
        for (u32 v = 0; v < viewRecords.size(); ++v)
        {
            const u32 first = static_cast<u32>(m_Jobs.size());
            for (u32 k = 0; k < m_Sets.size(); ++k)
            {
                const SetEntry& entry = m_Sets[k];
                if (!entry.Seated || (v > 0 && !entry.CastsShadows) ||
                    (views.FrustumCull && !Intersects(frusta[v], entry.WorldBounds)))
                {
                    continue;
                }
                const u32 levels = entry.SourceMesh->GetLodCount();
                const u32 subMeshes = static_cast<u32>(entry.SourceMesh->GetSubMeshes().size());
                m_Jobs.push_back(Job{
                    .View = v,
                    .Set = k,
                    .ThreadBase = m_ThreadCount,
                    .CommandBase = commandCount,
                    .ListBase = listWordBase + listWords,
                });
                m_ThreadCount += entry.Count;
                commandCount += levels * subMeshes;
                listWords += levels * entry.Count;
                draws += levels * subMeshes;
            }
            m_ViewJobs[v] = {first, static_cast<u32>(m_Jobs.size())};
        }

        m_JobsOffset = sizeof(GpuCullFrame);
        m_SetsOffset = m_JobsOffset + static_cast<u32>(m_Jobs.size() * sizeof(GpuCullJob));
        m_ViewsOffset = m_SetsOffset + static_cast<u32>(m_Sets.size() * sizeof(GpuCullSet));
        const u64 tableBytes = m_ViewsOffset + viewRecords.size() * sizeof(GpuCullView);

        // The level memory holds one entry per pooled record; a new buffer holds nothing yet.
        const u64 lodBytes = std::max<u64>(m_RecordCount, 1) * sizeof(u32);
        if (m_LodMemory == nullptr || m_LodMemory->GetSize() < lodBytes)
        {
            m_LodMemory = Buffer::Create(m_Context, {
                                                        .Name = "SceneRenderer Instanced Levels",
                                                        .Size = GrownSize(lodBytes, 4 * 1024),
                                                        .Usage = BufferUsage::Storage,
                                                        .DeviceLocal = true,
                                                    });
            m_LodHistoryValid = false;
        }
        if (m_HostLodMemory.size() != m_RecordCount || !m_LodHistoryValid)
        {
            m_HostLodMemory.assign(m_RecordCount, InstanceNoPreviousLod);
        }

        EnsureCapacity(slot,
                       static_cast<u64>(m_RecordCount) * sizeof(MeshInstance) +
                           static_cast<u64>(listWords) * sizeof(u32),
                       static_cast<u64>(commandCount) * sizeof(DrawIndexedIndirectCommand),
                       tableBytes);
        SyncRecords(slot);

        // Every command's range is the mesh's, its count zero for the cull to raise.
        auto* commands = static_cast<DrawIndexedIndirectCommand*>(slot.Commands->GetMappedData());
        slot.Jobs.clear();
        for (const Job& job : m_Jobs)
        {
            const SetEntry& entry = m_Sets[job.Set];
            const Mesh& mesh = *entry.SourceMesh;
            const u32 levels = mesh.GetLodCount();
            const u32 subMeshes = static_cast<u32>(mesh.GetSubMeshes().size());
            for (u32 l = 0; l < levels; ++l)
            {
                for (u32 s = 0; s < subMeshes; ++s)
                {
                    const MeshLodRange range = mesh.GetLodRange(l, s);
                    commands[job.CommandBase + l * subMeshes + s] = DrawIndexedIndirectCommand{
                        .IndexCount = range.IndexCount,
                        .InstanceCount = 0,
                        .FirstIndex = range.IndexOffset,
                        .VertexOffset = 0,
                        .FirstInstance = job.ListBase + l * entry.Count,
                    };
                }
            }
            slot.Jobs.push_back(JobRecord{
                .Set = entry.Owner,
                .View = job.View,
                .CommandBase = job.CommandBase,
                .LodCount = levels,
                .SubMeshCount = subMeshes,
                .Count = entry.Count,
                .ListBase = job.ListBase,
            });
        }

        // The tables the cull reads.
        auto* tables = static_cast<std::byte*>(slot.Tables->GetMappedData());
        const GpuCullFrame frame{
            .ReferenceViewProj = views.CameraViewProj,
            .PrevViewProj = views.PrevViewProj,
            .ReferenceEye = vec4(views.CameraPosition, views.HeightPixels),
            .HiZExtent = views.HiZExtent,
            .HiZTexture = views.HiZTexture,
            .OcclusionValid = views.OcclusionValid && m_GpuCulled ? 1u : 0u,
            // Absorbs the pyramid reduction's quantization, as the entity cull's does.
            .DepthBias = 0.001f,
        };
        std::memcpy(tables, &frame, sizeof(frame));
        for (usize j = 0; j < m_Jobs.size(); ++j)
        {
            const Job& job = m_Jobs[j];
            const GpuCullJob record{
                .ThreadBase = job.ThreadBase,
                .Set = job.Set,
                .View = job.View,
                .CommandBase = job.CommandBase,
                .ListBase = job.ListBase,
            };
            std::memcpy(tables + m_JobsOffset + j * sizeof(GpuCullJob), &record, sizeof(record));
        }
        for (usize k = 0; k < m_Sets.size(); ++k)
        {
            const SetEntry& entry = m_Sets[k];
            const AABB& bounds = entry.SourceMesh->GetBounds();
            const std::span<const f32> thresholds = entry.SourceMesh->GetLodScreenSizes();
            GpuCullSet record{
                .World = entry.World,
                .BoundsMin = vec4(bounds.Min, 0.0f),
                .BoundsMax = vec4(bounds.Max, 0.0f),
                .LodLow = vec4(0.0f),
                .LodHigh = vec4(0.0f),
                .RecordBase = entry.RecordBase,
                .Count = entry.Count,
                .LodCount = entry.SourceMesh->GetLodCount(),
                .SubMeshCount = static_cast<u32>(entry.SourceMesh->GetSubMeshes().size()),
                .MinPixels = entry.MinPixels,
                .MaxPixels = entry.MaxPixels,
                .MinShadowTexels = entry.MinShadowTexels,
                .ForcedLod = entry.ForcedLod,
                .LodMemoryBase = entry.RecordBase,
                .HistoryValid =
                    m_LodHistoryValid && std::ranges::find(m_PreviousMainSets, entry.Owner) !=
                                             m_PreviousMainSets.end()
                        ? 1u
                        : 0u,
            };
            for (usize t = 0; t < thresholds.size() && t < MaxMeshLods - 1; ++t)
            {
                (t < 4 ? record.LodLow : record.LodHigh)[static_cast<i32>(t % 4)] = thresholds[t];
            }
            std::memcpy(tables + m_SetsOffset + k * sizeof(GpuCullSet), &record, sizeof(record));
        }
        std::memcpy(tables + m_ViewsOffset, viewRecords.data(),
                    viewRecords.size() * sizeof(GpuCullView));

        if (!m_GpuCulled)
        {
            CullOnHost(slot, hostViews, views);
        }

        // The main view's level memory carries into the next frame for the sets it culled.
        m_PreviousMainSets.clear();
        const auto [mainFirst, mainEnd] = m_ViewJobs[0];
        for (u32 j = mainFirst; j < mainEnd; ++j)
        {
            m_PreviousMainSets.push_back(m_Sets[m_Jobs[j].Set].Owner);
        }
        m_LodHistoryValid = true;
        m_Prepared = true;

        u32 instances = 0;
        u32 seated = 0;
        for (const SetEntry& entry : m_Sets)
        {
            if (entry.Seated)
            {
                ++seated;
                instances += entry.Count;
            }
        }
        m_Stats.Sets = seated;
        m_Stats.Instances = instances;
        m_Stats.Views = static_cast<u32>(viewRecords.size());
        m_Stats.Jobs = static_cast<u32>(m_Jobs.size());
        m_Stats.Draws = draws;
        m_Stats.GpuCulled = m_GpuCulled;
        VE_PROFILE_COUNTER("Render/InstancedSets", static_cast<f64>(seated));
        VE_PROFILE_COUNTER("Render/InstancedInstances", static_cast<f64>(instances));
        VE_PROFILE_COUNTER("Render/InstancedDraws", static_cast<f64>(draws));
    }

    void InstancedMeshSystem::CullOnHost(FrameSlot& slot,
                                         const std::span<const InstancedCullView> hostViews,
                                         const InstancedFrameViews& views)
    {
        VE_PROFILE_SCOPE("Render/InstancedHostCull");
        auto* words = static_cast<u32*>(slot.Data->GetMappedData());
        auto* commands = static_cast<DrawIndexedIndirectCommand*>(slot.Commands->GetMappedData());
        const InstancedCullReference reference{
            .ViewProj = views.CameraViewProj,
            .Eye = views.CameraPosition,
            .HeightPixels = views.HeightPixels,
        };
        for (const Job& job : m_Jobs)
        {
            const SetEntry& entry = m_Sets[job.Set];
            const InstancedCullView& view = hostViews[job.View];
            const InstancedCullSet set{
                .World = entry.World,
                .MeshBounds = entry.SourceMesh->GetBounds(),
                .LodThresholds = entry.SourceMesh->GetLodScreenSizes(),
                .LodCount = entry.SourceMesh->GetLodCount(),
                .MinPixels = entry.MinPixels,
                .MaxPixels = entry.MaxPixels,
                .MinShadowTexels = entry.MinShadowTexels,
                .ForcedLod = entry.ForcedLod,
            };
            const bool history =
                m_LodHistoryValid &&
                std::ranges::find(m_PreviousMainSets, entry.Owner) != m_PreviousMainSets.end();
            const u32 subMeshes = static_cast<u32>(entry.SourceMesh->GetSubMeshes().size());
            const std::span<const MeshInstance> instances = entry.Store->GetInstances();
            for (u32 i = 0; i < entry.Count; ++i)
            {
                u8& memory = m_HostLodMemory[entry.RecordBase + i];
                const u8 previous = view.Main && history ? memory : InstanceNoPreviousLod;
                const InstancedCullVerdict verdict =
                    CullInstance(instances[i], set, view, reference, previous);
                if (view.Main)
                {
                    memory = verdict.Drawn ? static_cast<u8>(verdict.Lod) : InstanceNoPreviousLod;
                }
                if (!verdict.Drawn)
                {
                    continue;
                }
                const u32 commandBase = job.CommandBase + verdict.Lod * subMeshes;
                const u32 position = commands[commandBase].InstanceCount;
                for (u32 s = 0; s < subMeshes; ++s)
                {
                    ++commands[commandBase + s].InstanceCount;
                }
                words[job.ListBase + verdict.Lod * entry.Count + position] = i;
            }
        }
    }

    void InstancedMeshSystem::Import(RenderGraph& graph)
    {
        m_DataId = graph.ImportBuffer("SceneRenderer Instanced Data");
        m_CommandsId = graph.ImportBuffer("SceneRenderer Instanced Commands");
        m_LodId = graph.ImportBuffer("SceneRenderer Instanced Levels");
    }

    void InstancedMeshSystem::DeclareCull(RenderGraph& graph, const MipChainId& hiZ,
                                          const bool hiZSampled)
    {
        RenderGraph::PassBuilder builder = graph.AddComputePass("Scene Instanced Cull");
        builder.StorageBufferWrite(m_DataId)
            .StorageBufferWrite(m_CommandsId)
            .StorageBufferRead(m_LodId)
            .StorageBufferWrite(m_LodId);
        // The pyramid is read through its bindless slot; declaring the sample still drives its
        // transition into a sampled layout ahead of the cull.
        if (hiZSampled)
        {
            for (u32 level = 0; level < hiZ.LevelCount; ++level)
            {
                builder.Sample(hiZ.Level(level));
            }
        }
        builder.SkipWhen([this] { return !m_GpuCulled || m_ThreadCount == 0; });
        builder.Execute(
            [this](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                const FrameSlot& slot = m_Slots[m_FrameIndex];
                const u32 groups = (m_ThreadCount + CullGroupSize - 1) / CullGroupSize;
                const u32 groupsX = std::min(groups, MaxGroupsPerDimension);
                const u32 groupsY = (groups + groupsX - 1) / groupsX;
                cmd.BindPipeline(m_CullPipeline);
                m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {slot.CullSet},
                    .FirstSet = BindlessRegistry::FirstUserSet,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(CullPush{
                    .JobsOffset = m_JobsOffset,
                    .SetsOffset = m_SetsOffset,
                    .ViewsOffset = m_ViewsOffset,
                    .JobCount = static_cast<u32>(m_Jobs.size()),
                    .ThreadCount = m_ThreadCount,
                    .RowThreads = groupsX * CullGroupSize,
                });
                cmd.Dispatch(groupsX, groupsY, 1);
            });
    }

    void InstancedMeshSystem::DeclareReads(RenderGraph::PassBuilder& builder) const
    {
        builder.StorageBufferRead(m_DataId).IndirectRead(m_CommandsId);
    }

    void
    InstancedMeshSystem::AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const
    {
        const FrameSlot& slot = m_Slots[m_FrameIndex];
        VE_ASSERT(slot.Data != nullptr && m_LodMemory != nullptr,
                  "InstancedMeshSystem: the graph imports the instanced buffers on a frame no set "
                  "was prepared for");
        bindings.push_back({.Id = m_DataId, .Buffer = slot.Data});
        bindings.push_back({.Id = m_CommandsId, .Buffer = slot.Commands});
        bindings.push_back({.Id = m_LodId, .Buffer = m_LodMemory});
    }

    void InstancedMeshSystem::RecordJobDraws(CommandBuffer& cmd, const Job& job) const
    {
        const FrameSlot& slot = m_Slots[m_FrameIndex];
        const SetEntry& entry = m_Sets[job.Set];
        const u32 commandCount = entry.SourceMesh->GetLodCount() *
                                 static_cast<u32>(entry.SourceMesh->GetSubMeshes().size());
        if (m_GpuCulled)
        {
            cmd.DrawIndexedIndirect(slot.Commands,
                                    static_cast<u64>(job.CommandBase) *
                                        sizeof(DrawIndexedIndirectCommand),
                                    commandCount, sizeof(DrawIndexedIndirectCommand));
            return;
        }
        // The host cull's counts are known now, so only levels holding survivors draw.
        const auto* commands =
            static_cast<const DrawIndexedIndirectCommand*>(slot.Commands->GetMappedData());
        for (u32 c = 0; c < commandCount; ++c)
        {
            const DrawIndexedIndirectCommand& command = commands[job.CommandBase + c];
            if (command.InstanceCount > 0 && command.IndexCount > 0)
            {
                cmd.DrawIndexed(command.IndexCount, command.InstanceCount, command.FirstIndex,
                                command.VertexOffset, command.FirstInstance);
            }
        }
    }

    void InstancedMeshSystem::RecordGBuffer(CommandBuffer& cmd, const SurfacePush& push,
                                            const Ref<DescriptorSet>& drawDataSet) const
    {
        if (!m_Prepared || m_ViewJobs.empty())
        {
            return;
        }
        const FrameSlot& slot = m_Slots[m_FrameIndex];
        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const auto [first, end] = m_ViewJobs[0];
        for (u32 j = first; j < end; ++j)
        {
            const Job& job = m_Jobs[j];
            const SetEntry& entry = m_Sets[job.Set];
            entry.Material->BindInstanced(cmd);
            registry.Bind(cmd);
            cmd.BindDescriptorSets({drawDataSet.get()}, BindlessRegistry::FirstUserSet);
            SurfacePush setPush = push;
            setPush.InstanceBuffer = slot.DataHandle.Index;
            setPush.InstanceDraw = entry.DrawSlot;
            setPush.InstanceBase = entry.RecordBase;
            cmd.PushConstants(setPush);
            cmd.BindVertexBuffer(entry.SourceMesh->GetVertexBuffer());
            cmd.BindIndexBuffer(entry.SourceMesh->GetIndexBuffer());
            RecordJobDraws(cmd, job);
        }
    }

    bool InstancedMeshSystem::HasShadowDraws(const u32 shadowView) const
    {
        const u32 view = shadowView + 1;
        return m_Prepared && view < m_ViewJobs.size() &&
               m_ViewJobs[view].second > m_ViewJobs[view].first;
    }

    void InstancedMeshSystem::RecordShadowView(CommandBuffer& cmd, const u32 shadowView,
                                               const mat4& viewProj, const bool depthClamp) const
    {
        if (!HasShadowDraws(shadowView))
        {
            return;
        }
        const FrameSlot& slot = m_Slots[m_FrameIndex];
        cmd.BindPipeline(depthClamp ? m_ShadowClampPipeline : m_ShadowPipeline);
        m_Context.GetBindlessRegistry().Bind(cmd);
        const auto [first, end] = m_ViewJobs[shadowView + 1];
        for (u32 j = first; j < end; ++j)
        {
            const Job& job = m_Jobs[j];
            const SetEntry& entry = m_Sets[job.Set];
            cmd.PushConstants(ShadowInstancedPush{
                .ViewProjWorld = viewProj * entry.World,
                .InstanceBuffer = slot.DataHandle.Index,
                .InstanceBase = entry.RecordBase,
            });
            cmd.BindVertexBuffer(entry.SourceMesh->GetVertexBuffer());
            cmd.BindIndexBuffer(entry.SourceMesh->GetIndexBuffer());
            RecordJobDraws(cmd, job);
        }
    }

    void InstancedMeshSystem::ReadStats(const u32 frameIndex)
    {
        const FrameSlot& slot = m_Slots[frameIndex];
        m_Stats.PerView.clear();
        if (slot.Commands == nullptr)
        {
            return;
        }
        // The slot's fence has been waited, so the counts its last cull raised are final.
        const auto* commands =
            static_cast<const DrawIndexedIndirectCommand*>(slot.Commands->GetMappedData());
        u32 mainSurvivors = 0;
        for (const JobRecord& job : slot.Jobs)
        {
            InstancedMeshViewStats stats{.Set = job.Set, .View = job.View};
            for (u32 l = 0; l < job.LodCount && l < MaxMeshLods; ++l)
            {
                stats.Survivors[l] = commands[job.CommandBase + l * job.SubMeshCount].InstanceCount;
                if (job.View == 0)
                {
                    mainSurvivors += stats.Survivors[l];
                }
            }
            m_Stats.PerView.push_back(stats);
        }
        VE_PROFILE_COUNTER("Render/InstancedSurvivors", static_cast<f64>(mainSurvivors));
    }

    vector<InstancedMeshSurvivors> InstancedMeshSystem::ReadbackSurvivors() const
    {
        vector<InstancedMeshSurvivors> out;
        if (!m_Prepared)
        {
            return out;
        }
        // The buffers are host-visible: the device wait is what makes the cull's writes visible.
        m_Context.WaitIdle();
        const FrameSlot& slot = m_Slots[m_FrameIndex];
        const auto* words = static_cast<const u32*>(slot.Data->GetMappedData());
        const auto* commands =
            static_cast<const DrawIndexedIndirectCommand*>(slot.Commands->GetMappedData());
        for (const JobRecord& job : slot.Jobs)
        {
            InstancedMeshSurvivors survivors{.Set = job.Set, .View = job.View};
            for (u32 l = 0; l < job.LodCount; ++l)
            {
                const u32 count = commands[job.CommandBase + l * job.SubMeshCount].InstanceCount;
                for (u32 i = 0; i < count; ++i)
                {
                    survivors.Survivors.push_back(
                        {.Instance = words[job.ListBase + l * job.Count + i], .Lod = l});
                }
            }
            out.push_back(std::move(survivors));
        }
        return out;
    }
}
