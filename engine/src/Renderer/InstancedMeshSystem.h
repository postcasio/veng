#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/InstancedMesh.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Scene/Entity.h>

#include "LodSelection.h"
#include "ShadowCasters.h"

namespace Veng
{
    class AssetManager;
    class Mesh;
    class MaterialInstance;
    class Scene;
}

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class ComputePipeline;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class DrawBudget;
    class GraphicsPipeline;
    class PipelineLayout;
    struct GpuDrawData;
    struct InstancedCullView;
    struct SurfacePush;

    /// @brief What the frame's views are, for the per-instance cull.
    struct InstancedFrameViews
    {
        /// @brief The frame in flight being recorded.
        u32 FrameIndex = 0;
        /// @brief The main camera's unjittered world -> clip matrix.
        mat4 CameraViewProj{1.0f};
        /// @brief The main camera's position.
        vec3 CameraPosition{0.0f};
        /// @brief The main camera's rendered height in pixels.
        f32 HeightPixels = 1.0f;
        /// @brief Whether views cull by their frustum.
        bool FrustumCull = true;
        /// @brief The main view's level selection: bias and screen scale applied.
        LodView CameraLod;
        /// @brief The main view's level hysteresis.
        f32 Hysteresis = 0.0f;
        /// @brief The forced level every view draws at, or AutomaticLod.
        i32 ForcedLod = AutomaticLod;
        /// @brief The frame's shadow views, or null when no shadow pass runs.
        const ShadowCasterViews* Shadows = nullptr;
        /// @brief Whether the main view tests last frame's hi-Z pyramid.
        bool OcclusionValid = false;
        /// @brief Last frame's camera world -> clip, the pyramid's projection.
        mat4 PrevViewProj{1.0f};
        /// @brief The pyramid's level-0 extent.
        uvec2 HiZExtent{1};
        /// @brief The pyramid's bindless slot.
        u32 HiZTexture = 0;
    };

    /// @brief The renderer's instanced mesh sets: gather, upload, per-instance cull and draw.
    ///
    /// Each Execute gathers the scene's live InstancedMesh sets, gives each one draw slot for its
    /// frame and material, uploads what each set's store wrote since this frame slot last saw it,
    /// and culls every instance per view: one compute dispatch covering every (view, set) pair the
    /// frame keeps where the device supports GPU-driven culling (whatever the entity cull mode),
    /// else the same decisions on the host. A survivor lands in its level's list, and the g-buffer
    /// and shadow passes draw each level's list through one draw per level and submesh.
    ///
    /// Everything per frame — the instance records and the lists (one buffer), the indirect
    /// commands, the cull tables — has one copy per frame in flight, so the host writes a copy no
    /// frame still reads. The level memory the main view's hysteresis keeps is one device buffer
    /// the cull reads and writes each frame.
    class InstancedMeshSystem
    {
    public:
        /// @brief Creates the cull and shadow pipelines and the per-frame copies.
        /// @param context Render context.
        /// @param assets  Asset manager, for the cull and shadow shaders and the materials' set
        ///                pipelines.
        /// @return The system.
        static Unique<InstancedMeshSystem> Create(Context& context, AssetManager& assets);

        /// @brief Releases the instance buffers' bindless slots.
        ~InstancedMeshSystem();

        InstancedMeshSystem(const InstancedMeshSystem&) = delete;
        InstancedMeshSystem& operator=(const InstancedMeshSystem&) = delete;

        /// @brief Gathers the scene's drawable sets and their world bounds.
        ///
        /// A set is drawable with a resident, unskinned mesh, a resident material declaring
        /// "instancedSets", a non-empty store and its Visible flag set; one failing a material or
        /// mesh check is reported once and skipped.
        /// @param scene   The scene rendered.
        /// @param exclude An entity the view omits, as SceneView::Exclude.
        /// @param alpha   The view's interpolation alpha; a set's frame is drawn where its entity's
        ///                meshes would be.
        void Gather(const Scene& scene, Entity exclude, f32 alpha);

        /// @brief Whether the last Gather found a drawable set.
        [[nodiscard]] bool HasSets() const { return !m_Sets.empty(); }

        /// @brief The union of the gathered sets' world bounds.
        [[nodiscard]] const AABB& GetBounds() const { return m_Bounds; }

        /// @brief The union of the shadow-casting gathered sets' world bounds.
        [[nodiscard]] const AABB& GetCasterBounds() const { return m_CasterBounds; }

        /// @brief Gives each gathered set one draw slot and writes its DrawData record.
        ///
        /// A set the budget cannot seat is dropped for the frame and counted.
        /// @param drawData  The frame's DrawData region base pointer (already offset by frameBase).
        /// @param budget    The frame's draw budget, after the entity phases.
        /// @return The instances the seated sets carry.
        u32 ClaimDrawSlots(GpuDrawData* drawData, DrawBudget& budget);

        /// @brief Builds the frame's cull: views, jobs, uploads, commands; culls on the host when
        ///        the GPU path is not taken.
        /// @param views The frame's views.
        void PrepareFrame(const InstancedFrameViews& views);

        /// @brief Imports the per-frame buffers into the graph being built.
        /// @param graph The renderer's internal graph.
        void Import(RenderGraph& graph);

        /// @brief Declares the cull compute pass; the draws' passes must be declared after it.
        /// @param graph     The renderer's internal graph.
        /// @param hiZ       The hi-Z pyramid chain, sampled while @p hiZSampled.
        /// @param hiZSampled Whether the pyramid is held and the cull may test it.
        void DeclareCull(RenderGraph& graph, const MipChainId& hiZ, bool hiZSampled);

        /// @brief Declares a drawing pass's reads of the lists, records and commands.
        /// @param builder The drawing pass.
        void DeclareReads(RenderGraph::PassBuilder& builder) const;

        /// @brief Appends this frame's bindings of the imported buffers.
        /// @param bindings The frame's import bindings.
        void AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief Records the main view's set draws into the bound g-buffer attachments.
        /// @param cmd          The g-buffer pass's command buffer.
        /// @param push         The frame's surface push (frame base and view constants index).
        /// @param drawDataSet  The DrawData set the static draws bind at set 3.
        void RecordGBuffer(CommandBuffer& cmd, const SurfacePush& push,
                           const Ref<DescriptorSet>& drawDataSet) const;

        /// @brief Records one shadow view's set casters into its bound tile.
        /// @param cmd         The shadow pass's command buffer.
        /// @param shadowView  The view's index among the frame's shadow views.
        /// @param viewProj    The view's raw light-space view-projection.
        /// @param depthClamp  Whether the pass pancakes casters onto its near plane.
        void RecordShadowView(CommandBuffer& cmd, u32 shadowView, const mat4& viewProj,
                              bool depthClamp) const;

        /// @brief Whether any set casts into shadow view @p shadowView this frame.
        [[nodiscard]] bool HasShadowDraws(u32 shadowView) const;

        /// @brief Reads the survivors this frame slot's previous cull counted into the stats.
        /// @param frameIndex The frame in flight about to be recorded.
        void ReadStats(u32 frameIndex);

        /// @brief The last stats.
        [[nodiscard]] const InstancedMeshStats& GetStats() const { return m_Stats; }

        /// @brief Waits for the device and reads back every survivor of the last prepared frame.
        [[nodiscard]] vector<InstancedMeshSurvivors> ReadbackSurvivors() const;

        /// @brief Forces the host cull even where the device supports the GPU one.
        void SetForceCpuCull(bool force) { m_ForceCpuCull = force; }

        /// @brief Whether the last prepared frame culled on the GPU.
        [[nodiscard]] bool IsGpuCulled() const { return m_GpuCulled; }

    private:
        InstancedMeshSystem(Context& context, AssetManager& assets);

        /// @brief One gathered set.
        struct SetEntry
        {
            /// @brief The set's entity.
            Entity Owner;
            /// @brief Its world matrix this frame.
            mat4 World{1.0f};
            /// @brief Its world matrix last frame (World when first seen).
            mat4 PrevWorld{1.0f};
            /// @brief Its world bound.
            AABB WorldBounds = AABB::Empty();
            /// @brief Its resident mesh.
            const Mesh* SourceMesh = nullptr;
            /// @brief Its resident material.
            const MaterialInstance* Material = nullptr;
            /// @brief Its store.
            const InstanceSet* Store = nullptr;
            /// @brief Its drawn count.
            u32 Count = 0;
            /// @brief Whether it casts shadows.
            bool CastsShadows = true;
            /// @brief The size band.
            f32 MinPixels = 0.0f;
            /// @brief The size band's upper end.
            f32 MaxPixels = 0.0f;
            /// @brief The minimum shadow caster size.
            f32 MinShadowTexels = 0.0f;
            /// @brief Its entity's forced level, or AutomaticLod.
            i32 ForcedLod = AutomaticLod;
            /// @brief Its DrawData slot, region-relative; valid when Seated.
            u32 DrawSlot = 0;
            /// @brief Whether ClaimDrawSlots seated it.
            bool Seated = false;
            /// @brief Its first record in the pooled store layout.
            u32 RecordBase = 0;
        };

        /// @brief One (view, set) pair culled per instance.
        struct Job
        {
            /// @brief The view: 0 the main camera, 1 + n shadow view n.
            u32 View = 0;
            /// @brief Index into m_Sets.
            u32 Set = 0;
            /// @brief First dispatch invocation.
            u32 ThreadBase = 0;
            /// @brief First command; level l, submesh s is CommandBase + l * submeshes + s.
            u32 CommandBase = 0;
            /// @brief Absolute word index of level 0's list in the data buffer.
            u32 ListBase = 0;
        };

        /// @brief What the stats read back needs of one job, kept per frame slot.
        struct JobRecord
        {
            /// @brief The set's entity.
            Entity Set;
            /// @brief The view.
            u32 View = 0;
            /// @brief The job's first command.
            u32 CommandBase = 0;
            /// @brief Levels the set's mesh carries.
            u32 LodCount = 1;
            /// @brief Submeshes it carries.
            u32 SubMeshCount = 1;
            /// @brief Its drawn count.
            u32 Count = 0;
            /// @brief Absolute word index of level 0's list.
            u32 ListBase = 0;
        };

        /// @brief One frame in flight's copies.
        struct FrameSlot
        {
            /// @brief The instance records, then the lists.
            Ref<Buffer> Data;
            /// @brief Data's bindless storage slot.
            StorageBufferHandle DataHandle;
            /// @brief The indirect commands.
            Ref<Buffer> Commands;
            /// @brief The cull tables.
            Ref<Buffer> Tables;
            /// @brief The cull's set over Data, Commands, the level memory and Tables.
            Ref<DescriptorSet> CullSet;
            /// @brief The level memory CullSet binds.
            const Buffer* BoundLod = nullptr;
            /// @brief The pooled layout serial Data's records were last fully written under.
            u64 SyncedLayout = 0;
            /// @brief Per set (in m_Sets order), the store write serial last uploaded.
            vector<u64> SyncedWrites;
            /// @brief The jobs the slot's last prepared frame culled.
            vector<JobRecord> Jobs;
        };

        /// @brief Grows a slot's buffers to hold the frame, rebuilding its cull set on change.
        void EnsureCapacity(FrameSlot& slot, u64 dataBytes, u64 commandBytes, u64 tableBytes);

        /// @brief Brings a slot's records current with the stores.
        void SyncRecords(FrameSlot& slot);

        /// @brief Runs the frame's cull on the host into a slot's lists and commands.
        void CullOnHost(FrameSlot& slot, std::span<const InstancedCullView> hostViews,
                        const InstancedFrameViews& views);

        /// @brief Records a job's draws: indirect on the GPU path, direct on the host one.
        void RecordJobDraws(CommandBuffer& cmd, const Job& job) const;

        /// @brief Builds the depth-only set pipeline, with or without depth clamp.
        Ref<GraphicsPipeline> BuildShadowPipeline(AssetManager& assets, bool depthClamp);

        Context& m_Context;
        AssetManager& m_Assets;

        /// @brief The cull compute pipeline and its layout.
        Ref<ComputePipeline> m_CullPipeline;
        /// @brief The cull pipeline's layout.
        Ref<PipelineLayout> m_CullLayout;
        /// @brief The cull's set-3 layout.
        Ref<DescriptorSetLayout> m_CullSetLayout;
        /// @brief The depth-only set pipelines' layout (bindless only, the push block).
        Ref<PipelineLayout> m_ShadowLayout;
        /// @brief The depth-only set pipeline without depth clamp (punctual faces).
        Ref<GraphicsPipeline> m_ShadowPipeline;
        /// @brief The depth-only set pipeline with depth clamp where the device has it (cascades).
        Ref<GraphicsPipeline> m_ShadowClampPipeline;

        /// @brief One copy per frame in flight.
        vector<FrameSlot> m_Slots;
        /// @brief The main view's level memory, one u32 per pooled record.
        Ref<Buffer> m_LodMemory;
        /// @brief The host cull's level memory, one byte per pooled record.
        vector<u8> m_HostLodMemory;

        /// @brief The gathered sets, this frame.
        vector<SetEntry> m_Sets;
        /// @brief Last frame's world matrix per set entity, for velocity.
        vector<std::pair<Entity, mat4>> m_PreviousWorlds;
        /// @brief The gathered sets' world bound.
        AABB m_Bounds = AABB::Empty();
        /// @brief The shadow-casting gathered sets' world bound.
        AABB m_CasterBounds = AABB::Empty();
        /// @brief Each set's store id and layout serial, the pooled layout's identity.
        vector<std::pair<u64, u64>> m_LayoutKey;
        /// @brief Moves each time the pooled layout changes.
        u64 m_LayoutSerial = 1;
        /// @brief The pooled record count.
        u32 m_RecordCount = 0;
        /// @brief Whether the level memory holds last frame's levels for the current layout.
        bool m_LodHistoryValid = false;
        /// @brief The sets the main view culled per instance last frame, whose levels it kept.
        vector<Entity> m_PreviousMainSets;
        /// @brief Entities already reported for an unusable material or mesh.
        vector<Entity> m_Reported;

        /// @brief The frame's jobs.
        vector<Job> m_Jobs;
        /// @brief Per view (0 main, 1 + n shadow view n), the range of m_Jobs it holds.
        vector<std::pair<u32, u32>> m_ViewJobs;
        /// @brief Invocations the frame's jobs cover.
        u32 m_ThreadCount = 0;
        /// @brief Byte offsets of the job, set and view tables in the frame's table buffer.
        u32 m_JobsOffset = 0;
        /// @brief The set table's offset.
        u32 m_SetsOffset = 0;
        /// @brief The view table's offset.
        u32 m_ViewsOffset = 0;
        /// @brief The frame slot prepared last.
        u32 m_FrameIndex = 0;
        /// @brief Whether the frame culls on the GPU.
        bool m_GpuCulled = false;
        /// @brief Forces the host cull.
        bool m_ForceCpuCull = false;
        /// @brief Whether a frame has been prepared since creation.
        bool m_Prepared = false;

        /// @brief The imported data buffer.
        ResourceId m_DataId;
        /// @brief The imported command buffer.
        ResourceId m_CommandsId;
        /// @brief The imported level memory.
        ResourceId m_LodId;

        /// @brief The stats.
        InstancedMeshStats m_Stats;
    };
}
