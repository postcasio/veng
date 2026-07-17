#include "ShadowSystem.h"

#include <algorithm>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ShadowCascades.h>

#include "GpuBlocks.h"

namespace Veng::Renderer
{
    namespace
    {
        // The punctual shadow atlas tile grid: CubeFaceCount columns × MaxShadowedPunctual
        // rows of PunctualShadowResolution² tiles. A shadowed light's slot s, face f maps
        // to tile (column f, row s) — linear index s·CubeFaceCount + f — so a spot uses
        // tile (0, s) and a point uses the whole of row s.
        constexpr u32 PunctualAtlasColumns = CubeFaceCount;
        constexpr u32 PunctualAtlasRows = MaxShadowedPunctual;
    }

    Unique<ShadowSystem> ShadowSystem::Create(Context& context,
                                              const SceneRendererSettings& settings)
    {
        return Unique<ShadowSystem>(new ShadowSystem(context, settings));
    }

    ShadowSystem::ShadowSystem(Context& context, const SceneRendererSettings& settings)
        : m_Context(context)
    {
        m_FramesInFlight = m_Context.GetMaxFramesInFlight();

        // Immutable comparison sampler for hardware SampleCmp: LESS-or-equal, linear
        // filter for the hardware 2×2 PCF. The MoltenVK argument-buffer restriction
        // applies only to set-0 bindless arrays; a dedicated set 1 sampler is fine.
        m_ComparisonSampler =
            Sampler::Create(m_Context, {
                                           .Name = "SceneRenderer Shadow Comparison Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Nearest,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                           .CompareEnable = true,
                                           .CompareOp = CompareOp::LessOrEqual,
                                           .BorderColor = BorderColor::OpaqueWhite,
                                       });

        // Set 1 — the shadow system:
        //   0: directional cascade atlas (SampledImage)
        //   1: shared immutable comparison sampler (baked into the layout)
        //   2: ShadowConstants dynamic uniform (ring-buffered)
        //   3: PunctualShadowBlock dynamic uniform (ring-buffered)
        //   4: punctual shadow atlas (SampledImage)
        // The comparison sampler is shared across cascade and punctual tiles; binding 1
        // is immutable, so descriptor writes supply only bindings 0, 4 and buffers 2, 3.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Shadow Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment,
                                    .ImmutableSamplers = {m_ComparisonSampler}},
                                   {.Binding = 2,
                                    .Type = DescriptorType::UniformBufferDynamic,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 3,
                                    .Type = DescriptorType::UniformBufferDynamic,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 4,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });

        // Debug shadow-blit set: atlas (binding 0) + ordinary sampler (binding 1) for raw depth.
        m_BlitSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Shadow Blit Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        // Ordinary clamp sampler for raw-depth debug reads; rewritten into each rebuilt blit set.
        m_BlitSampler = Sampler::Create(m_Context, {
                                                       .Name = "SceneRenderer Shadow Blit Sampler",
                                                       .MagFilter = Filter::Nearest,
                                                       .MinFilter = Filter::Nearest,
                                                       .MipmapMode = MipmapMode::Nearest,
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge,
                                                       .AnisotropyEnabled = false,
                                                   });

        // 1×1 D32 dummy atlas cleared to depth = 1 (full visibility), bound when no
        // shadow pass is wired so the layout is always satisfied. Transitioned to
        // ShaderReadOnly immediately so the lighting pass samples a valid layout even
        // when it does not declare .Sample on it.
        m_DummyImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Dummy Shadow",
                                         .Extent = {1, 1, 1},
                                         .Format = Format::D32Sfloat,
                                         .Usage = ImageUsage::DepthAttachment | ImageUsage::Sampled,
                                     });
        m_DummyView = ImageView::Create(m_Context, {
                                                       .Name = "SceneRenderer Dummy Shadow View",
                                                       .Image = m_DummyImage,
                                                   });
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(m_Context);
                const ResourceId target = graph.Import("Dummy Shadow");
                graph.AddPass("Clear Dummy Shadow")
                    .Depth({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = m_DummyView};
                graph.Compile()->Execute(cmd, {&binding, 1});
                cmd.PrepareForAccess(m_DummyView, AccessKind::Sample);
            });

        // ShadowConstants ring: framesInFlight regions, each aligned to
        // minUniformBufferOffsetAlignment. Dynamic offset at bind time = frame * stride.
        const u64 minAlign =
            GetVkPhysicalDevice(m_Context).getProperties().limits.minUniformBufferOffsetAlignment;
        const u64 blockSize = sizeof(ShadowConstantsBlock);
        const u64 alignment = minAlign == 0 ? 1 : minAlign;
        m_ConstantsRingStride =
            static_cast<u32>(((blockSize + alignment - 1) / alignment) * alignment);
        VE_ASSERT(m_ConstantsRingStride % alignment == 0,
                  "ShadowConstants ring stride {} is not a multiple of "
                  "minUniformBufferOffsetAlignment {}",
                  m_ConstantsRingStride, alignment);

        m_ConstantsBuffer = Buffer::Create(
            m_Context, {
                           .Name = "SceneRenderer ShadowConstants",
                           .Size = static_cast<u64>(m_ConstantsRingStride) * m_FramesInFlight,
                           .Usage = BufferUsage::Uniform,
                           .HostMapped = true,
                       });

        // Zero all regions: w = 0 in ShadowParams means shadows disabled.
        std::memset(m_ConstantsBuffer->GetMappedData(), 0,
                    static_cast<usize>(m_ConstantsRingStride) * m_FramesInFlight);

        // PunctualShadowBlock ring: same alignment and frame*stride dynamic offset as
        // the ShadowConstants ring.
        const u64 punctualBlockSize = sizeof(PunctualShadowBlock);
        m_PunctualRingStride =
            static_cast<u32>(((punctualBlockSize + alignment - 1) / alignment) * alignment);
        VE_ASSERT(m_PunctualRingStride % alignment == 0,
                  "PunctualShadowBlock ring stride {} is not a multiple of "
                  "minUniformBufferOffsetAlignment {}",
                  m_PunctualRingStride, alignment);

        m_PunctualBuffer = Buffer::Create(
            m_Context, {
                           .Name = "SceneRenderer PunctualShadows",
                           .Size = static_cast<u64>(m_PunctualRingStride) * m_FramesInFlight,
                           .Usage = BufferUsage::Uniform,
                           .HostMapped = true,
                       });

        // Zero all regions: Params.x type = 0 means "no map", so all lights read full visibility.
        std::memset(m_PunctualBuffer->GetMappedData(), 0,
                    static_cast<usize>(m_PunctualRingStride) * m_FramesInFlight);

        CreatePunctualAtlas(settings);
    }

    // The shadow system holds no set-0 bindless slots (all of set 1 is off bindless), so the
    // destructor releases none; the owned Refs retire through the per-frame bin as they drop.
    ShadowSystem::~ShadowSystem() = default;

    u32 ShadowSystem::GetMaxShadowResolution(Context& context)
    {
        // The directional atlas is widest at the largest cascade grid (2×2 at four
        // cascades), so a tile larger than the device limit / 2 would overflow it.
        const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(MaxCascades);
        const u32 factor = std::max(grid.Columns, grid.Rows);
        return context.GetMaxImageDimension2D() / factor;
    }

    u32 ShadowSystem::GetMaxPunctualShadowResolution(Context& context)
    {
        // The punctual atlas tiles CubeFaceCount columns × MaxShadowedPunctual rows,
        // so its widest side is CubeFaceCount · resolution.
        const u32 factor = std::max(CubeFaceCount, MaxShadowedPunctual);
        return context.GetMaxImageDimension2D() / factor;
    }

    void ShadowSystem::ClampResolutions(Context& context, SceneRendererSettings& settings)
    {
        settings.ShadowResolution =
            std::min(settings.ShadowResolution, GetMaxShadowResolution(context));
        settings.PunctualShadowResolution =
            std::min(settings.PunctualShadowResolution, GetMaxPunctualShadowResolution(context));
    }

    void ShadowSystem::Reconfigure(const SceneRendererSettings& settings)
    {
        CreatePunctualAtlas(settings);
    }

    void ShadowSystem::CreatePunctualAtlas(const SceneRendererSettings& settings)
    {
        // 2D depth atlas of CubeFaceCount × MaxShadowedPunctual tiles.
        const u32 res = settings.PunctualShadowResolution;
        const uvec2 atlasExtent{PunctualAtlasColumns * res, PunctualAtlasRows * res};

        m_PunctualImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Punctual Shadow Atlas",
                                         .Extent = {atlasExtent.x, atlasExtent.y, 1},
                                         .Format = Format::D32Sfloat,
                                         .Usage = ImageUsage::DepthAttachment | ImageUsage::Sampled,
                                     });
        m_PunctualView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer Punctual Shadow Atlas View",
                                             .Image = m_PunctualImage,
                                         });

        // Clear to depth = 1 (full visibility) and transition to ShaderReadOnly so
        // binding 4 is in a valid sampleable layout before the punctual pass runs.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(m_Context);
                const ResourceId target = graph.Import("Clear Punctual Atlas");
                graph.AddPass("Clear Punctual Shadow Atlas")
                    .Depth({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = m_PunctualView};
                graph.Compile()->Execute(cmd, {&binding, 1});
                cmd.PrepareForAccess(m_PunctualView, AccessKind::Sample);
            });
    }

    void ShadowSystem::RebuildSets(const Ref<ImageView>& atlasView)
    {
        // Fresh sets every rebuild: the prior sets may still be referenced by an
        // in-flight command buffer, and the bindings carry no update-after-bind flags
        // (set 1 stays out of Metal argument buffers for the comparison sampler), so
        // they are never updated in place. The old sets retire through the per-frame
        // bin once their last frame's fence signals.
        m_Set = DescriptorSet::Create(m_Context, {
                                                     .Name = "SceneRenderer Shadow Set",
                                                     .Layout = m_SetLayout,
                                                 });
        m_Set->Write(0, atlasView);
        m_Set->Write(2, m_ConstantsBuffer, 0, sizeof(ShadowConstantsBlock));
        m_Set->Write(3, m_PunctualBuffer, 0, sizeof(PunctualShadowBlock));
        m_Set->Write(4, m_PunctualView);

        m_BlitSet = DescriptorSet::Create(m_Context, {
                                                         .Name = "SceneRenderer Shadow Blit Set",
                                                         .Layout = m_BlitSetLayout,
                                                     });
        m_BlitSet->Write(0, atlasView);
        m_BlitSet->Write(1, m_BlitSampler);
    }

    void ShadowSystem::WriteFrameConstants(u32 frameIndex, const ShadowConstantsBlock& constants,
                                           const PunctualShadowBlock& punctual)
    {
        // Write only the current frame's region (not yet submitted; safe). These rings are
        // framesInFlight-deep, so they index by the frame-in-flight — not the shared
        // view-constants slot; the bind selects it via dynamic offset frame * stride.
        std::memcpy(static_cast<u8*>(m_ConstantsBuffer->GetMappedData()) +
                        static_cast<usize>(frameIndex) * m_ConstantsRingStride,
                    &constants, sizeof(ShadowConstantsBlock));

        // Flush punctual records into this frame's binding-3 region (same safe write).
        // Unused slots stay zeroed → type 0 = "no map".
        std::memcpy(static_cast<u8*>(m_PunctualBuffer->GetMappedData()) +
                        static_cast<usize>(frameIndex) * m_PunctualRingStride,
                    &punctual, sizeof(PunctualShadowBlock));
    }
}
