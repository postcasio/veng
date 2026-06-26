#include "DebugDrawScenePass.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>

#include <cstring>

namespace Veng::Renderer
{
    namespace
    {
        // The debug-draw shader ids in the engine core pack (auto-mounted by AssetManager).
        constexpr AssetId DebugLineVertId{0xA0C295968AB1FB31ULL};
        constexpr AssetId DebugLineFragId{0x6DC2A1A30445E9EAULL};
        constexpr AssetId DebugBillboardVertId{0x3819703AFD880100ULL};
        constexpr AssetId DebugBillboardFragId{0x3535322F615A85DAULL};

        // An occluded gizmo fades to this fraction of its color rather than vanishing.
        constexpr f32 OccludedFade = 0.25f;

        // Shared push block for both debug pipelines (matches debug_line/debug_billboard .slang).
        struct DebugPushConstants
        {
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 Pad0;
            f32 OccludedFade;
            u32 Pad1;
            u32 Pad2;
            u32 Pad3;
        };

        // One GPU billboard record (std430): vec4 fields first, then the texture index + pad.
        // Matches debug_billboard.vert's GpuBillboard byte-for-byte (48 bytes).
        struct GpuBillboard
        {
            vec4 PositionSize;
            vec4 Color;
            u32 Texture;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        constexpr u32 BillboardVertexCount = 6;
        constexpr u32 MaxLineVertices = 1 << 16;
        constexpr u32 MaxBillboards = 4096;

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, AssetId id, const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "DebugDrawScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }
    }

    DebugDrawScenePass::DebugDrawScenePass(Context& context, AssetManager& assets,
                                           const DebugDraw* accumulator, const Format outputFormat,
                                           const SamplerHandle samplerHandle,
                                           const u32 framesInFlight, const uvec2 extent)
        : m_Context(context), m_Accumulator(accumulator), m_OutputFormat(outputFormat),
          m_SamplerHandle(samplerHandle), m_FramesInFlight(framesInFlight), m_Extent(extent)
    {
        const AssetHandle<Veng::Shader> lineVs =
            LoadShader(assets, DebugLineVertId, "debug-line vertex");
        const AssetHandle<Veng::Shader> lineFs =
            LoadShader(assets, DebugLineFragId, "debug-line fragment");
        const AssetHandle<Veng::Shader> billboardVs =
            LoadShader(assets, DebugBillboardVertId, "debug-billboard vertex");
        const AssetHandle<Veng::Shader> billboardFs =
            LoadShader(assets, DebugBillboardFragId, "debug-billboard fragment");

        const PushConstantRange pushRange =
            PushConstantRange::Of<DebugPushConstants>(ShaderStage::Vertex | ShaderStage::Fragment);

        // Alpha-blend over the tonemapped scene color: gizmos composite, the occluded fade lowers
        // alpha so a hidden gizmo reads dimmer.
        const PipelineAttachmentInfo blendTarget{.Format = m_OutputFormat,
                                                 .Blend = BlendState::AlphaBlend()};

        // The line vertex shader names the debug-line vertex layout (position + color).
        optional<VertexBufferLayout> lineLayout;
        const ShaderInterface& lineVsInterface = lineVs.Get()->Interface;
        if (lineVsInterface.VertexLayoutId.has_value())
        {
            const AssetResult<AssetHandle<Veng::VertexLayout>> layoutResult =
                assets.LoadSync<Veng::VertexLayout>(*lineVsInterface.VertexLayoutId);
            VE_ASSERT(layoutResult.has_value(),
                      "DebugDrawScenePass: line vertex layout load failed: {}",
                      layoutResult.error().Detail);
            lineLayout = layoutResult->Get()->GetLayout();
        }

        m_LineLayout = PipelineLayout::Create(m_Context, {
                                                             .Name = "DebugDraw Line Layout",
                                                             .PushConstantRanges = {pushRange},
                                                         });
        m_LinePipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "DebugDraw Line Pipeline",
                           .ColorAttachments = {blendTarget},
                           .VertexBufferLayout = lineLayout,
                           .PipelineLayout = m_LineLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = lineVs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = lineFs.Get()->Module},
                               },
                           .Topology = PrimitiveTopology::LineList,
                       });

        // The billboard set: binding 0 the per-frame record SSBO (set 1, off bindless — a closed
        // per-frame producer→consumer buffer needs no global registration).
        m_BillboardSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "DebugDraw Billboard Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_BillboardLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "DebugDraw Billboard Layout",
                                                  .DescriptorSetLayouts = {m_BillboardSetLayout},
                                                  .PushConstantRanges = {pushRange},
                                              });
        m_BillboardPipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "DebugDraw Billboard Pipeline",
                .ColorAttachments = {blendTarget},
                .PipelineLayout = m_BillboardLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = billboardVs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = billboardFs.Get()->Module},
                    },
            });
        m_BillboardSet = DescriptorSet::Create(m_Context, {
                                                              .Name = "DebugDraw Billboard Set",
                                                              .Layout = m_BillboardSetLayout,
                                                          });

        // Host-mapped, ring-buffered line vertex + billboard buffers: one region per
        // frame-in-flight so the GPU never reads a region the host is rewriting.
        m_LineRegionStride = static_cast<u64>(MaxLineVertices) * sizeof(DebugLineVertex);
        m_LineBuffer = Buffer::Create(m_Context, {
                                                     .Name = "DebugDraw Line Buffer",
                                                     .Size = m_LineRegionStride * m_FramesInFlight,
                                                     .Usage = BufferUsage::Vertex,
                                                     .HostMapped = true,
                                                 });

        m_BillboardRegionStride = static_cast<u64>(MaxBillboards) * sizeof(GpuBillboard);
        m_BillboardBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "DebugDraw Billboard Buffer",
                                          .Size = m_BillboardRegionStride * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });
    }

    DebugDrawScenePass::~DebugDrawScenePass() = default;

    void DebugDrawScenePass::Resize(const uvec2 extent)
    {
        m_Extent = extent;
    }

    u32 DebugDrawScenePass::UploadLines()
    {
        const vector<DebugLineVertex>& vertices = m_Accumulator->GetLineVertices();
        if (vertices.empty())
        {
            return 0;
        }

        const u32 count = std::min(static_cast<u32>(vertices.size()), MaxLineVertices);
        const u32 region = m_Context.GetCurrentFrameInFlight();
        auto* base = static_cast<u8*>(m_LineBuffer->GetMappedData()) +
                     static_cast<usize>(region) * m_LineRegionStride;
        std::memcpy(base, vertices.data(), static_cast<usize>(count) * sizeof(DebugLineVertex));
        return count;
    }

    u32 DebugDrawScenePass::UploadBillboards()
    {
        const vector<DebugBillboard>& billboards = m_Accumulator->GetBillboards();
        if (billboards.empty())
        {
            return 0;
        }

        const u32 count = std::min(static_cast<u32>(billboards.size()), MaxBillboards);
        const u32 region = m_Context.GetCurrentFrameInFlight();
        const u64 regionOffset = static_cast<u64>(region) * m_BillboardRegionStride;
        auto* base = static_cast<u8*>(m_BillboardBuffer->GetMappedData()) + regionOffset;

        for (u32 i = 0; i < count; ++i)
        {
            const DebugBillboard& src = billboards[i];
            GpuBillboard record{
                .PositionSize = vec4(src.WorldPosition, src.Size),
                .Color = src.Color,
                .Texture = src.Texture.Index,
            };
            std::memcpy(base + static_cast<usize>(i) * sizeof(GpuBillboard), &record,
                        sizeof(GpuBillboard));
        }

        // Bind this frame's region as the whole SSBO range for the draw.
        m_BillboardSet->Write(0, m_BillboardBuffer, regionOffset, m_BillboardRegionStride);
        return count;
    }

    void DebugDrawScenePass::Declare(RenderGraph& graph, const PassIO& io)
    {
        const ResourceId outputId = io.Output;
        const ResourceId depthId = io.GBufferDepth;
        const TextureHandle depthHandle = io.DepthHandle;
        const SamplerHandle samplerHandle = m_SamplerHandle;

        graph
            .AddPass("DebugDraw")
            // Composite over the tonemapped scene color; the depth sample is the only occlusion test.
            .Color({
                .Resource = outputId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            })
            .Sample(depthId)
            .Execute(
                [this, depthHandle, samplerHandle](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();

                    if (m_Accumulator == nullptr || m_Accumulator->IsEmpty())
                    {
                        return;
                    }

                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const u32 lineCount = UploadLines();
                    const u32 billboardCount = UploadBillboards();

                    const DebugPushConstants push{
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .OccludedFade = OccludedFade,
                    };

                    // The debug pass runs at full output extent (overlay content stays crisp).
                    cmd.SetViewport({0, 0}, m_Extent);
                    cmd.SetScissor({0, 0}, m_Extent);

                    if (lineCount > 0)
                    {
                        cmd.BindPipeline(m_LinePipeline);
                        registry.Bind(cmd);
                        cmd.PushConstants(push);

                        const u32 region = m_Context.GetCurrentFrameInFlight();
                        // Bind the whole buffer; the per-frame region base is the firstVertex.
                        cmd.BindVertexBuffer(m_LineBuffer);
                        cmd.Draw(lineCount, 1, region * MaxLineVertices, 0);
                    }

                    if (billboardCount > 0)
                    {
                        cmd.BindPipeline(m_BillboardPipeline);
                        registry.Bind(cmd);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {m_BillboardSet},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Graphics,
                        });
                        cmd.PushConstants(push);
                        // A single non-instanced draw of 6 vertices per record: the vertex stage
                        // derives the record/corner from SV_VertexID (avoiding SV_InstanceID, whose
                        // base-instance lowering needs the DrawParameters capability).
                        cmd.Draw(BillboardVertexCount * billboardCount, 1, 0, 0);
                    }
                });
    }
}
