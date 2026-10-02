#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Scene/Visibility.h>

#include "DrawGather.h"
#include "DrawPlan.h"

namespace Veng
{
    class Mesh;
}

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;

    /// @brief One visible mesh's per-frame record, std430-identical to Veng/depth_caster.slang's CasterRecord.
    ///
    /// The world matrix places an instanced depth draw; the three normal columns carry the
    /// inverse-transpose of its upper 3×3 for a pass that writes a world normal.
    struct GpuCasterRecord
    {
        /// @brief The mesh's world matrix.
        mat4 World;
        /// @brief Column 0 of the world normal matrix (xyz).
        vec4 NormalColumn0;
        /// @brief Column 1 of the world normal matrix (xyz).
        vec4 NormalColumn1;
        /// @brief Column 2 of the world normal matrix (xyz).
        vec4 NormalColumn2;
    };

    static_assert(sizeof(GpuCasterRecord) == 112,
                  "GpuCasterRecord must match the shader CasterRecord (112 bytes)");

    /// @brief The frame's caster records: one per visible mesh, written once and read by every depth pass.
    ///
    /// Record i belongs to SceneView::Visible[i], so a depth pass's instance id for a submesh
    /// candidate is its MeshCandidate. Each frame in flight owns its own buffer and descriptor set,
    /// grown when the scene outgrows it; the buffer the frame index names is free to rewrite
    /// because that frame's fence has been waited.
    class CasterRecordRing
    {
    public:
        /// @brief Allocates each frame's buffer at a starting capacity and writes its set.
        /// @param context        Renderer context.
        /// @param framesInFlight Number of frames the ring spans.
        CasterRecordRing(Context& context, u32 framesInFlight);
        ~CasterRecordRing();

        CasterRecordRing(const CasterRecordRing&) = delete;
        CasterRecordRing& operator=(const CasterRecordRing&) = delete;

        /// @brief Writes one record per visible mesh into a frame's buffer, growing it if needed.
        /// @param frameIndex The frame in flight being recorded.
        /// @param visible    This frame's visible meshes, in the order the instance ids index.
        void Write(u32 frameIndex, std::span<const VisibleMesh> visible);

        /// @brief The set (one vertex-stage storage buffer) holding a frame's records.
        [[nodiscard]] const DescriptorSet& GetSet(u32 frameIndex) const;

        /// @brief The layout a depth pipeline declares at its first author set to read the records.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const { return m_SetLayout; }

    private:
        /// @brief One frame in flight's records.
        struct Frame
        {
            /// @brief Host-mapped storage buffer of Capacity records.
            Ref<Buffer> Records;
            /// @brief The set binding Records.
            Ref<DescriptorSet> Set;
            /// @brief Records the buffer holds.
            u32 Capacity = 0;
        };

        /// @brief (Re)allocates a frame's buffer and set at a capacity.
        void Allocate(Frame& frame, u32 capacity);

        /// @brief Renderer context.
        Context& m_Context;
        /// @brief The records set's layout.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief One entry per frame in flight.
        vector<Frame> m_Frames;
    };

    /// @brief One instanced depth draw of a view: a submesh's index range over a run of instance ids.
    struct DepthDraw
    {
        /// @brief The mesh whose buffers the draw binds.
        const Mesh* SourceMesh = nullptr;
        /// @brief Indices the submesh draws.
        u32 IndexCount = 0;
        /// @brief The submesh's first index.
        u32 FirstIndex = 0;
        /// @brief The draw's first instance: its offset into the instance-id list.
        u32 FirstInstance = 0;
        /// @brief Instances the draw covers.
        u32 InstanceCount = 0;
    };

    /// @brief Static depth casters across several views, grouped once and filtered per view.
    ///
    /// A pass opens each view and adds the casters that view keeps. A caster any view keeps joins
    /// one shared list, and each view only sets its bit in that caster's mask. Build sorts the
    /// shared list by (mesh, submesh) with SortDrawKeys and cuts it with GroupContiguousSlots
    /// once, then walks it a single time to write every view's instance ids, view after view.
    /// A view's ids keep the shared order, so its draws are exactly those its own casters would
    /// group into alone, while the sort and the grouping run once however many views share them. An
    /// instance id is the caster-record index (the candidate's MeshCandidate). Pure CPU: the
    /// lists are kept across frames, so a steady state allocates nothing.
    class DepthCasterGrouping
    {
    public:
        /// @brief Views one grouping holds: a caster's mask is one 64-bit word.
        static constexpr u32 MaxViews = 64;

        /// @brief Clears the previous frame's views and starts this frame's.
        /// @param candidates The broadphase's submesh candidates the added ids index.
        void Begin(std::span<const SubMeshCandidate> candidates);

        /// @brief Opens a view.
        /// @return The view's index, counted from zero in the order views are opened.
        /// @pre Fewer than MaxViews views are open.
        u32 AddView();

        /// @brief Adds a static caster a view keeps.
        /// @param view         The view keeping it, from AddView.
        /// @param candidateId  The caster's broadphase candidate id.
        /// @param mesh         The candidate's mesh.
        /// @param subMeshIndex The candidate's submesh.
        /// @pre A candidate is added to one view at most once.
        void Add(u32 view, u32 candidateId, const Mesh& mesh, u32 subMeshIndex);

        /// @brief Groups the shared casters and lays out every view's draws and instance ids.
        void Build();

        /// @brief Number of views opened since Begin.
        [[nodiscard]] u32 GetViewCount() const { return static_cast<u32>(m_Views.size()); }

        /// @brief A view's draws, in (mesh, submesh) order.
        /// @pre Build ran.
        [[nodiscard]] std::span<const DepthDraw> GetViewDraws(u32 view) const;

        /// @brief Every view's instance ids, view after view; a draw's FirstInstance indexes it.
        /// @pre Build ran.
        [[nodiscard]] std::span<const u32> GetInstanceIds() const { return m_InstanceIds; }

    private:
        /// @brief One view's range of draws.
        struct View
        {
            /// @brief First draw of the view in m_Draws.
            u32 FirstDraw = 0;
            /// @brief Draws the view holds.
            u32 DrawCount = 0;
        };

        /// @brief Marks a candidate not in the shared list.
        static constexpr u32 NotShared = ~0u;

        /// @brief The candidates the current frame's keys index.
        std::span<const SubMeshCandidate> m_Candidates;
        /// @brief Per candidate, its index in m_Masks, or NotShared.
        vector<u32> m_SharedIndex;
        /// @brief The shared casters' keys, in the order they joined; sorted by Build.
        vector<DrawKey> m_Keys;
        /// @brief Per shared caster (in joining order), the views keeping it, one bit each.
        vector<u64> m_Masks;
        /// @brief The sorted shared casters as slots; a slot's CandidateId is its position.
        vector<DrawSlot> m_Slots;
        /// @brief The shared slots' groups, one per mesh.
        vector<DrawGroup> m_Groups;
        /// @brief The shared slots' runs, one per submesh.
        vector<InstanceRun> m_Runs;
        /// @brief Every view's draws, view after view.
        vector<DepthDraw> m_Draws;
        /// @brief Every view's instance ids, view after view.
        vector<u32> m_InstanceIds;
        /// @brief The open views.
        vector<View> m_Views;
    };

    /// @brief Static depth-caster draws across views, as instanced runs over one instance-id upload.
    ///
    /// Wraps a DepthCasterGrouping with the GPU half: Upload copies every view's instance ids into
    /// one per-frame buffer, which rides per-instance vertex binding 1, and RecordView issues one
    /// instanced draw per submesh at the view's offset into it. Each frame in flight owns its own
    /// host-mapped buffer, grown on demand.
    class DepthInstanceBatch
    {
    public:
        /// @brief Constructs an empty batch.
        /// @param context        Renderer context.
        /// @param name           Debug name for the instance-id buffers.
        /// @param framesInFlight Number of frames the instance-id buffers span.
        DepthInstanceBatch(Context& context, string name, u32 framesInFlight);
        ~DepthInstanceBatch();

        DepthInstanceBatch(const DepthInstanceBatch&) = delete;
        DepthInstanceBatch& operator=(const DepthInstanceBatch&) = delete;

        /// @brief Clears the previous frame's views and starts this frame's.
        /// @param candidates The broadphase's submesh candidates the added ids index.
        void Begin(std::span<const SubMeshCandidate> candidates) { m_Grouping.Begin(candidates); }

        /// @brief Opens a view; see DepthCasterGrouping::AddView.
        /// @return The view's index.
        u32 AddView() { return m_Grouping.AddView(); }

        /// @brief Adds a static caster a view keeps; see DepthCasterGrouping::Add.
        /// @param view         The view keeping it.
        /// @param candidateId  The caster's broadphase candidate id.
        /// @param mesh         The candidate's mesh.
        /// @param subMeshIndex The candidate's submesh.
        void Add(const u32 view, const u32 candidateId, const Mesh& mesh, const u32 subMeshIndex)
        {
            m_Grouping.Add(view, candidateId, mesh, subMeshIndex);
        }

        /// @brief Groups the casters and lays out every view's draws.
        void Build() { m_Grouping.Build(); }

        /// @brief Copies this frame's instance ids to the GPU, growing the frame's buffer if needed.
        /// @param frameIndex The frame in flight being recorded.
        /// @pre Build ran this frame.
        void Upload(u32 frameIndex);

        /// @brief Binds the uploaded instance ids at the per-instance binding.
        /// @pre Upload ran this frame and the batch is not empty.
        void BindInstanceIds(CommandBuffer& cmd) const;

        /// @brief Records one view's draws: each mesh's buffers once, then one draw per submesh.
        /// @pre The view's pipeline, sets, push block and the instance ids are bound.
        void RecordView(CommandBuffer& cmd, u32 view) const;

        /// @brief Whether no view holds a draw.
        [[nodiscard]] bool IsEmpty() const { return m_Grouping.GetInstanceIds().empty(); }

        /// @brief Number of draws a view holds.
        [[nodiscard]] u32 GetViewDrawCount(const u32 view) const
        {
            return static_cast<u32>(m_Grouping.GetViewDraws(view).size());
        }

    private:
        /// @brief Renderer context.
        Context& m_Context;
        /// @brief Debug name for the instance-id buffers.
        string m_Name;
        /// @brief The CPU grouping the buffers are written from.
        DepthCasterGrouping m_Grouping;
        /// @brief Each frame in flight's instance-id buffer.
        vector<Ref<Buffer>> m_IdBuffers;
        /// @brief Each frame in flight's buffer capacity, in ids.
        vector<u32> m_IdCapacity;
        /// @brief The frame in flight the last Upload wrote.
        u32 m_UploadedFrame = 0;
    };
}
