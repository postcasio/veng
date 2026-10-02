#include "DepthInstancing.h"

#include <array>
#include <bit>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Assert.h>
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

    void DepthCasterGrouping::Begin(const std::span<const SubMeshCandidate> candidates)
    {
        m_Candidates = candidates;
        m_SharedIndex.assign(candidates.size(), NotShared);
        m_Keys.clear();
        m_Masks.clear();
        m_Slots.clear();
        m_Groups.clear();
        m_Runs.clear();
        m_Draws.clear();
        m_InstanceIds.clear();
        m_Views.clear();
    }

    u32 DepthCasterGrouping::AddView()
    {
        VE_ASSERT(m_Views.size() < MaxViews, "DepthCasterGrouping: more than {} views", MaxViews);
        m_Views.emplace_back();
        return static_cast<u32>(m_Views.size()) - 1;
    }

    void DepthCasterGrouping::Add(const u32 view, const u32 candidateId, const Mesh& mesh,
                                  const u32 subMeshIndex)
    {
        u32& shared = m_SharedIndex[candidateId];
        if (shared == NotShared)
        {
            shared = static_cast<u32>(m_Masks.size());
            m_Masks.push_back(0);
            m_Keys.push_back(DrawKey{
                .Pipeline = nullptr,
                .SourceMesh = &mesh,
                .SubMeshIndex = subMeshIndex,
                .Candidate = candidateId,
            });
        }
        m_Masks[shared] |= u64{1} << view;
    }

    void DepthCasterGrouping::Build()
    {
        SortDrawKeys(m_Keys);

        // The shared slots' CandidateIds are their positions, so a submesh's casters are
        // consecutive and GroupContiguousSlots cuts one run per submesh.
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
                .CandidateId = static_cast<u32>(m_Slots.size()),
            });
        }
        GroupContiguousSlots(m_Slots, m_Groups, m_Runs);

        // Size every view's draws and instances, so the second walk writes each in place.
        const u32 viewCount = static_cast<u32>(m_Views.size());
        std::array<u32, MaxViews> drawCursor{};
        std::array<u32, MaxViews> instanceCursor{};
        for (const InstanceRun& run : m_Runs)
        {
            u64 runMask = 0;
            for (u32 s = run.FirstSlot; s < run.FirstSlot + run.Count; ++s)
            {
                const u64 mask = m_Masks[m_SharedIndex[m_Keys[s].Candidate]];
                runMask |= mask;
                for (u64 bits = mask; bits != 0; bits &= bits - 1)
                {
                    ++instanceCursor[std::countr_zero(bits)];
                }
            }
            for (u64 bits = runMask; bits != 0; bits &= bits - 1)
            {
                ++drawCursor[std::countr_zero(bits)];
            }
        }
        u32 drawTotal = 0;
        u32 instanceTotal = 0;
        for (u32 v = 0; v < viewCount; ++v)
        {
            m_Views[v] = View{.FirstDraw = drawTotal, .DrawCount = drawCursor[v]};
            drawTotal += drawCursor[v];
            drawCursor[v] = m_Views[v].FirstDraw;
            const u32 instances = instanceCursor[v];
            instanceCursor[v] = instanceTotal;
            instanceTotal += instances;
        }
        m_Draws.resize(drawTotal);
        m_InstanceIds.resize(instanceTotal);

        std::array<u32, MaxViews> runStart{};
        for (const InstanceRun& run : m_Runs)
        {
            u64 runMask = 0;
            for (u32 s = run.FirstSlot; s < run.FirstSlot + run.Count; ++s)
            {
                const DrawKey& key = m_Keys[s];
                const u64 mask = m_Masks[m_SharedIndex[key.Candidate]];
                for (u64 bits = mask & ~runMask; bits != 0; bits &= bits - 1)
                {
                    const u32 v = static_cast<u32>(std::countr_zero(bits));
                    runStart[v] = instanceCursor[v];
                }
                runMask |= mask;
                const u32 record = m_Candidates[key.Candidate].MeshCandidate;
                for (u64 bits = mask; bits != 0; bits &= bits - 1)
                {
                    m_InstanceIds[instanceCursor[std::countr_zero(bits)]++] = record;
                }
            }
            const DrawSlot& first = m_Slots[run.FirstSlot];
            for (u64 bits = runMask; bits != 0; bits &= bits - 1)
            {
                const u32 v = static_cast<u32>(std::countr_zero(bits));
                m_Draws[drawCursor[v]++] = DepthDraw{
                    .SourceMesh = first.SourceMesh,
                    .IndexCount = first.IndexCount,
                    .FirstIndex = first.FirstIndex,
                    .FirstInstance = runStart[v],
                    .InstanceCount = instanceCursor[v] - runStart[v],
                };
            }
        }
    }

    std::span<const DepthDraw> DepthCasterGrouping::GetViewDraws(const u32 view) const
    {
        const View& range = m_Views[view];
        return std::span<const DepthDraw>(m_Draws).subspan(range.FirstDraw, range.DrawCount);
    }

    DepthInstanceBatch::DepthInstanceBatch(Context& context, string name, const u32 framesInFlight)
        : m_Context(context), m_Name(std::move(name)), m_IdBuffers(framesInFlight),
          m_IdCapacity(framesInFlight, 0)
    {
    }

    DepthInstanceBatch::~DepthInstanceBatch() = default;

    void DepthInstanceBatch::Upload(const u32 frameIndex)
    {
        m_UploadedFrame = frameIndex;
        const std::span<const u32> ids = m_Grouping.GetInstanceIds();
        const u32 count = static_cast<u32>(ids.size());
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
        std::memcpy(m_IdBuffers[frameIndex]->GetMappedData(), ids.data(),
                    static_cast<usize>(count) * sizeof(u32));
    }

    void DepthInstanceBatch::BindInstanceIds(CommandBuffer& cmd) const
    {
        cmd.BindInstanceBuffer(m_IdBuffers[m_UploadedFrame]);
    }

    void DepthInstanceBatch::RecordView(CommandBuffer& cmd, const u32 view) const
    {
        // Draws come in mesh order, so each mesh's buffers bind once per view.
        const Mesh* bound = nullptr;
        for (const DepthDraw& draw : m_Grouping.GetViewDraws(view))
        {
            if (draw.SourceMesh != bound)
            {
                cmd.BindVertexBuffer(draw.SourceMesh->GetVertexBuffer());
                cmd.BindIndexBuffer(draw.SourceMesh->GetIndexBuffer());
                bound = draw.SourceMesh;
            }
            cmd.DrawIndexed(draw.IndexCount, draw.InstanceCount, draw.FirstIndex, 0,
                            draw.FirstInstance);
        }
    }
}
