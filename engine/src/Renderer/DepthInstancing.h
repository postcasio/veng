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

    /// @brief One depth pass's static-caster draws across its views, as instanced runs.
    ///
    /// A pass adds each view's static survivors, closes the view, and once every view is built
    /// uploads the instance ids and records view by view. Closing a view orders its draws by
    /// (mesh, submesh) with SortDrawKeys and appends each draw's caster-record index to the
    /// instance-id list, so the instances of one submesh are contiguous; GroupContiguousSlots then
    /// cuts them into groups (one buffer bind per mesh) and runs (one instanced draw per
    /// submesh). The ids ride per-instance vertex binding 1, each frame in flight in its own
    /// host-mapped buffer grown on demand. The CPU lists are kept across frames, so a steady
    /// state allocates nothing.
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
        void Begin(std::span<const SubMeshCandidate> candidates);

        /// @brief Adds one static draw to the view being built.
        /// @param candidateId  The survivor's broadphase candidate id.
        /// @param mesh         The candidate's mesh.
        /// @param subMeshIndex The candidate's submesh.
        void Add(u32 candidateId, const Mesh& mesh, u32 subMeshIndex);

        /// @brief Closes the view being built: sorts its draws and lays out their runs.
        void EndView();

        /// @brief Copies this frame's instance ids to the GPU, growing the frame's buffer if needed.
        /// @param frameIndex The frame in flight being recorded.
        void Upload(u32 frameIndex);

        /// @brief Binds the uploaded instance ids at the per-instance binding.
        /// @pre Upload ran this frame and the batch is not empty.
        void BindInstanceIds(CommandBuffer& cmd) const;

        /// @brief Records one view's runs: each group's mesh buffers once, then one draw per run.
        /// @pre The view's pipeline, sets, push block and the instance ids are bound.
        void RecordView(CommandBuffer& cmd, u32 view) const;

        /// @brief Whether no view holds a draw.
        [[nodiscard]] bool IsEmpty() const { return m_InstanceIds.empty(); }

        /// @brief Number of draws a view holds.
        [[nodiscard]] u32 GetViewDrawCount(u32 view) const { return m_Views[view].SlotCount; }

    private:
        /// @brief One closed view's range of slots and groups.
        struct View
        {
            /// @brief First slot of the view in m_Slots.
            u32 FirstSlot = 0;
            /// @brief Slots the view holds.
            u32 SlotCount = 0;
            /// @brief First group of the view in m_Groups.
            u32 FirstGroup = 0;
            /// @brief Groups the view holds.
            u32 GroupCount = 0;
        };

        /// @brief Renderer context.
        Context& m_Context;
        /// @brief Debug name for the instance-id buffers.
        string m_Name;
        /// @brief The candidates the current frame's keys index.
        std::span<const SubMeshCandidate> m_Candidates;
        /// @brief The view being built's keys; cleared when it closes.
        vector<DrawKey> m_Keys;
        /// @brief Every view's slots; a slot's CandidateId is its instance-id offset.
        vector<DrawSlot> m_Slots;
        /// @brief Every view's groups; a group's slot indices are relative to its view.
        vector<DrawGroup> m_Groups;
        /// @brief Every view's runs, indexed by the groups.
        vector<InstanceRun> m_Runs;
        /// @brief The caster-record index of every instance, in slot order.
        vector<u32> m_InstanceIds;
        /// @brief The closed views, in the order they were built.
        vector<View> m_Views;
        /// @brief Each frame in flight's instance-id buffer.
        vector<Ref<Buffer>> m_IdBuffers;
        /// @brief Each frame in flight's buffer capacity, in ids.
        vector<u32> m_IdCapacity;
        /// @brief The frame in flight the last Upload wrote.
        u32 m_UploadedFrame = 0;
    };
}
