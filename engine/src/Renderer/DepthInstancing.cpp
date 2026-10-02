#include "DepthInstancing.h"

#include "Passes/GBufferScenePass.h"

#include <bit>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>

namespace Veng::Renderer
{
    namespace
    {
        // The records each frame starts with, so an ordinary scene never grows its buffer and the
        // set is always written before its first bind.
        constexpr u32 InitialCasterCapacity = 1024;

        // The instance ids each frame's buffer starts with.
        constexpr u32 InitialInstanceCapacity = 4096;

        // The smallest power of two holding at least `needed`, never below `floor`.
        u32 GrownCapacity(const u32 needed, const u32 floor)
        {
            return std::bit_ceil(needed > floor ? needed : floor);
        }
    }

    CasterRecordRing::CasterRecordRing(Context& context, const u32 framesInFlight)
        : m_Context(context)
    {
        // Vertex stage only: the depth vertex shaders are the record's one reader.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Caster Record Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_Frames.resize(framesInFlight);
        for (Frame& frame : m_Frames)
        {
            Allocate(frame, InitialCasterCapacity);
        }
    }

    CasterRecordRing::~CasterRecordRing() = default;

    void CasterRecordRing::Allocate(Frame& frame, const u32 capacity)
    {
        frame.Records = Buffer::Create(
            m_Context, {
                           .Name = "Caster Records",
                           .Size = static_cast<u64>(capacity) * sizeof(GpuCasterRecord),
                           .Usage = BufferUsage::Storage,
                           .HostMapped = true,
                       });
        // A fresh set rather than a rewrite: an earlier frame's commands may still hold the old one.
        frame.Set = DescriptorSet::Create(m_Context, {
                                                         .Name = "Caster Record Set",
                                                         .Layout = m_SetLayout,
                                                     });
        frame.Set->Write(0, frame.Records);
        frame.Capacity = capacity;
    }

    void CasterRecordRing::Write(const u32 frameIndex, const std::span<const VisibleMesh> visible)
    {
        Frame& frame = m_Frames[frameIndex];
        const u32 count = static_cast<u32>(visible.size());
        if (count > frame.Capacity)
        {
            Allocate(frame, GrownCapacity(count, InitialCasterCapacity));
        }

        auto* records = static_cast<GpuCasterRecord*>(frame.Records->GetMappedData());
        for (u32 i = 0; i < count; ++i)
        {
            const VisibleMesh& item = visible[i];
            records[i] = GpuCasterRecord{
                .World = item.World,
                .NormalColumn0 = vec4(item.NormalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(item.NormalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(item.NormalMatrix[2], 0.0f),
            };
        }
    }

    const DescriptorSet& CasterRecordRing::GetSet(const u32 frameIndex) const
    {
        return *m_Frames[frameIndex].Set;
    }

    DepthInstanceBatch::DepthInstanceBatch(Context& context, string name, const u32 framesInFlight)
        : m_Context(context), m_Name(std::move(name)), m_IdBuffers(framesInFlight),
          m_IdCapacity(framesInFlight, 0)
    {
    }

    DepthInstanceBatch::~DepthInstanceBatch() = default;

    void DepthInstanceBatch::Begin(const std::span<const SubMeshCandidate> candidates)
    {
        m_Candidates = candidates;
        m_Keys.clear();
        m_Slots.clear();
        m_Groups.clear();
        m_Runs.clear();
        m_InstanceIds.clear();
        m_Views.clear();
    }

    void DepthInstanceBatch::Add(const u32 candidateId, const Mesh& mesh, const u32 subMeshIndex)
    {
        m_Keys.push_back(DrawKey{
            .Pipeline = nullptr,
            .SourceMesh = &mesh,
            .SubMeshIndex = subMeshIndex,
            .Candidate = candidateId,
        });
    }

    void DepthInstanceBatch::EndView()
    {
        SortDrawKeys(m_Keys);

        const u32 firstSlot = static_cast<u32>(m_Slots.size());
        for (const DrawKey& key : m_Keys)
        {
            const SubMesh& subMesh = key.SourceMesh->GetSubMeshes()[key.SubMeshIndex];
            m_Slots.push_back(DrawSlot{
                .SourceMesh = key.SourceMesh,
                .Pipeline = nullptr,
                .PipelineKey = nullptr,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = static_cast<u32>(m_InstanceIds.size()),
            });
            m_InstanceIds.push_back(m_Candidates[key.Candidate].MeshCandidate);
        }
        m_Keys.clear();

        const u32 slotCount = static_cast<u32>(m_Slots.size()) - firstSlot;
        const u32 firstGroup = static_cast<u32>(m_Groups.size());
        GroupContiguousSlots(std::span<const DrawSlot>(m_Slots).subspan(firstSlot, slotCount),
                             m_Groups, m_Runs);
        m_Views.push_back(View{
            .FirstSlot = firstSlot,
            .SlotCount = slotCount,
            .FirstGroup = firstGroup,
            .GroupCount = static_cast<u32>(m_Groups.size()) - firstGroup,
        });
    }

    void DepthInstanceBatch::Upload(const u32 frameIndex)
    {
        m_UploadedFrame = frameIndex;
        const u32 count = static_cast<u32>(m_InstanceIds.size());
        if (count == 0)
        {
            return;
        }

        // This frame's buffer was last read by the frame whose fence has been waited, so it is
        // replaced or overwritten freely.
        if (count > m_IdCapacity[frameIndex])
        {
            const u32 capacity = GrownCapacity(count, InitialInstanceCapacity);
            m_IdBuffers[frameIndex] =
                Buffer::Create(m_Context, {
                                              .Name = fmt::format("{} Instance Ids", m_Name),
                                              .Size = static_cast<u64>(capacity) * sizeof(u32),
                                              .Usage = BufferUsage::Vertex,
                                              .HostMapped = true,
                                          });
            m_IdCapacity[frameIndex] = capacity;
        }
        std::memcpy(m_IdBuffers[frameIndex]->GetMappedData(), m_InstanceIds.data(),
                    static_cast<usize>(count) * sizeof(u32));
    }

    void DepthInstanceBatch::BindInstanceIds(CommandBuffer& cmd) const
    {
        cmd.BindInstanceBuffer(m_IdBuffers[m_UploadedFrame]);
    }

    void DepthInstanceBatch::RecordView(CommandBuffer& cmd, const u32 view) const
    {
        const View& range = m_Views[view];
        const std::span<const DrawSlot> slots =
            std::span<const DrawSlot>(m_Slots).subspan(range.FirstSlot, range.SlotCount);
        for (u32 g = 0; g < range.GroupCount; ++g)
        {
            // Groups split on the mesh alone (the key carries no pipeline), so each binds its own.
            const DrawGroup& group = m_Groups[range.FirstGroup + g];
            cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
            cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
            RecordInstanceRuns(cmd, slots, m_Runs, group);
        }
    }
}
