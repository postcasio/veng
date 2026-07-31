#include "DofChain.h"

#include <algorithm>
#include <array>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    namespace
    {
        // The fullscreen vertex stage the composite shares with the other fullscreen passes.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId DofCocPrefilterCompId{0x19AE4B252B75EAF5ULL};
        constexpr AssetId DofTileDilateCompId{0x08133BD4AA68833CULL};
        constexpr AssetId DofGatherCompId{0xD602DA47084ECA46ULL};
        constexpr AssetId DofFillCompId{0x318351739B93477BULL};
        constexpr AssetId DofCompositeFragId{0x27F453A760B77C78ULL};

        // Linear float HDR for the layers, the gather/fill targets, the tile records, and the
        // composite result.
        constexpr Format DofFormat = Format::RGBA16Sfloat;
        // Signed radius in R, view-space depth in G — the tile reduction's only source.
        constexpr Format DofCocFormat = Format::RG16Sfloat;

        // The circle-of-confusion prefilter push, matching dof_coc_prefilter.comp.
        struct DofPrefilterPush
        {
            uvec2 DestExtent;
            vec2 ScaleUV;
            vec2 MaxUV;
            vec2 DepthParams;
            f32 Aperture;
            f32 FocusDistance;
            f32 CocScale;
            f32 MaxCoc;
        };

        // The tile reduction push, matching dof_tile_dilate.comp.
        struct DofTilePush
        {
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The ring-gather push, matching dof_gather.comp.
        struct DofGatherPush
        {
            uvec2 DestExtent;
            u32 RingCount;
            u32 NearField;
            f32 MaxCoc;
            f32 Pad0;
        };

        // The fill push, matching dof_fill.comp.
        struct DofFillPush
        {
            uvec2 DestExtent;
        };

        // This frame's valid sub-rect mapping over an allocation, as the shaders consume it:
        // xy the validExtent/allocExtent scale, zw the half-texel-inset clamp.
        struct SubRectUv
        {
            vec2 Scale;
            vec2 Max;
        };

        [[nodiscard]] SubRectUv ComputeSubRectUv(const uvec2 valid, const uvec2 alloc)
        {
            const vec2 validF{static_cast<f32>(valid.x), static_cast<f32>(valid.y)};
            const vec2 allocF{static_cast<f32>(std::max(alloc.x, 1u)),
                              static_cast<f32>(std::max(alloc.y, 1u))};
            return SubRectUv{
                .Scale = validF / allocF,
                .Max = (validF - vec2(0.5f)) / allocF,
            };
        }

        [[nodiscard]] uvec2 TileGridOf(const uvec2 halfExtent)
        {
            return glm::max(uvec2(1), (halfExtent + (DofTileSize - 1)) / DofTileSize);
        }
    }

    Unique<DofChain> DofChain::Create(Context& context, AssetManager& assets,
                                      const Ref<DescriptorSetLayout>& bloomDownUpLayout,
                                      const Format compositeFormat)
    {
        return Unique<DofChain>(new DofChain(context, assets, bloomDownUpLayout, compositeFormat));
    }

    DofChain::DofChain(Context& context, AssetManager& assets,
                       const Ref<DescriptorSetLayout>& bloomDownUpLayout,
                       const Format compositeFormat)
        : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "DofChain: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> prefilterCs =
            LoadShader(DofCocPrefilterCompId, "DoF CoC prefilter");
        const AssetHandle<Veng::Shader> tileCs = LoadShader(DofTileDilateCompId, "DoF tile dilate");
        const AssetHandle<Veng::Shader> gatherCs = LoadShader(DofGatherCompId, "DoF ring gather");
        const AssetHandle<Veng::Shader> fillCs = LoadShader(DofFillCompId, "DoF fill");
        const AssetHandle<Veng::Shader> compositeFs =
            LoadShader(DofCompositeFragId, "DoF composite fragment");

        m_PrefilterSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DoF Prefilter Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 4,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 5,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_GatherSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DoF Gather Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });

        m_PrefilterLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DoF Prefilter Layout",
                           .DescriptorSetLayouts = {m_PrefilterSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<DofPrefilterPush>(
                               ShaderStage::Compute)},
                       });
        m_PrefilterPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Prefilter Pipeline",
                .PipelineLayout = m_PrefilterLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = prefilterCs.Get()->Module},
            });

        m_TileLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Tile Layout",
                .DescriptorSetLayouts = {bloomDownUpLayout},
                .PushConstantRanges = {PushConstantRange::Of<DofTilePush>(ShaderStage::Compute)},
            });
        m_TilePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Tile Pipeline",
                .PipelineLayout = m_TileLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = tileCs.Get()->Module},
            });

        m_GatherLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Gather Layout",
                .DescriptorSetLayouts = {m_GatherSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<DofGatherPush>(ShaderStage::Compute)},
            });
        m_GatherPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Gather Pipeline",
                .PipelineLayout = m_GatherLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = gatherCs.Get()->Module},
            });

        m_FillLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Fill Layout",
                .DescriptorSetLayouts = {bloomDownUpLayout},
                .PushConstantRanges = {PushConstantRange::Of<DofFillPush>(ShaderStage::Compute)},
            });
        m_FillPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Fill Pipeline",
                .PipelineLayout = m_FillLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = fillCs.Get()->Module},
            });

        m_CompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DoF Composite Layout",
                           .PushConstantRanges = {PushConstantRange::Of<DofCompositePush>(
                               ShaderStage::Fragment)},
                       });
        m_CompositePipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer DoF Composite Pipeline",
                .ColorAttachments = {{.Format = compositeFormat}},
                .PipelineLayout = m_CompositeLayout,
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                        {.Stage = ShaderStage::Fragment, .Module = compositeFs.Get()->Module},
                    },
            });

        // Every stage reads its sources through one linear clamp-to-edge sampler: the layers and
        // tile records are sampled at fractional offsets, and clamping is what keeps a kernel that
        // reaches past the valid sub-rect from wrapping.
        const SharedSampler shared = m_Context.GetBindlessRegistry().AcquireSampler({
            .Name = "SceneRenderer DoF Sampler",
            .MagFilter = Filter::Linear,
            .MinFilter = Filter::Linear,
            .MipmapMode = MipmapMode::Nearest,
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
            .AnisotropyEnabled = false,
        });
        m_Sampler = shared.Sampler;
        m_SamplerHandle = shared.Handle;
    }

    DofChain::~DofChain()
    {
        ReleaseHandles();
    }

    void DofChain::ReleaseHandles()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_CocHandle);
        bindless.Release(m_NearFillHandle);
        bindless.Release(m_FarFillHandle);
        bindless.Release(m_SceneHandle);
        m_CocHandle = {};
        m_NearFillHandle = {};
        m_FarFillHandle = {};
        m_SceneHandle = {};
    }

    uvec2 DofChain::GetHalfExtent() const
    {
        return DofHalfExtent(m_Extent);
    }

    void DofChain::Recreate(const SceneRendererSettings& settings, const uvec2 extent,
                            const Ref<ImageView>& hdrView, const Ref<ImageView>& depthView,
                            const Ref<DescriptorSetLayout>& bloomDownUpLayout)
    {
        m_Extent = glm::max(uvec2(1), extent);
        ReleaseHandles();

        // The targets exist only when depth of field runs (the toggle in the Final view, or the
        // CoC debug arm, which force-wires the first two stages).
        const bool composited = settings.Mode == DebugView::Final && settings.DepthOfField;
        const bool wanted = composited || settings.Mode == DebugView::CoC;
        if (!wanted)
        {
            m_NearImage.reset();
            m_NearView.reset();
            m_FarImage.reset();
            m_FarView.reset();
            m_CocImage.reset();
            m_CocView.reset();
            m_TileImage.reset();
            m_TileView.reset();
            m_NearBlurImage.reset();
            m_NearBlurView.reset();
            m_FarBlurImage.reset();
            m_FarBlurView.reset();
            m_NearFillImage.reset();
            m_NearFillView.reset();
            m_FarFillImage.reset();
            m_FarFillView.reset();
            m_SceneImage.reset();
            m_SceneView.reset();
            m_PrefilterSet.reset();
            m_TileSet.reset();
            m_NearGatherSet.reset();
            m_FarGatherSet.reset();
            m_NearFillSet.reset();
            m_FarFillSet.reset();
            return;
        }

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        const uvec2 half = DofHalfExtent(m_Extent);
        const uvec2 tiles = TileGridOf(half);

        // Allocates one half-resolution storage/sampled target and its view.
        auto MakeHalfTarget =
            [&](const char* name, const Format format, Ref<Image>& image, Ref<ImageView>& view)
        {
            image = Image::Create(m_Context, {
                                                 .Name = name,
                                                 .Extent = {half.x, half.y, 1},
                                                 .Format = format,
                                                 .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                             });
            view = ImageView::Create(m_Context, {.Name = name, .Image = image});
        };

        MakeHalfTarget("SceneRenderer DoF Near", DofFormat, m_NearImage, m_NearView);
        MakeHalfTarget("SceneRenderer DoF Far", DofFormat, m_FarImage, m_FarView);
        MakeHalfTarget("SceneRenderer DoF CoC", DofCocFormat, m_CocImage, m_CocView);
        MakeHalfTarget("SceneRenderer DoF Near Blur", DofFormat, m_NearBlurImage, m_NearBlurView);
        MakeHalfTarget("SceneRenderer DoF Far Blur", DofFormat, m_FarBlurImage, m_FarBlurView);
        MakeHalfTarget("SceneRenderer DoF Near Fill", DofFormat, m_NearFillImage, m_NearFillView);
        MakeHalfTarget("SceneRenderer DoF Far Fill", DofFormat, m_FarFillImage, m_FarFillView);

        m_TileImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer DoF Tiles",
                                         .Extent = {tiles.x, tiles.y, 1},
                                         .Format = DofFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });
        m_TileView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer DoF Tiles View", .Image = m_TileImage});

        // The lit scene-color intermediate exists only when the composite runs: the tail writes
        // here instead of the HDR target and the composite writes the HDR target, so bloom,
        // metering, and tonemap read the id they always did. The debug arm skips it and prefilters
        // the HDR target directly.
        if (composited)
        {
            m_SceneImage = Image::Create(
                m_Context, {
                               .Name = "SceneRenderer DoF Scene",
                               .Extent = {m_Extent.x, m_Extent.y, 1},
                               .Format = DofFormat,
                               .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                           });
            m_SceneView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer DoF Scene View", .Image = m_SceneImage});
        }
        else
        {
            m_SceneImage.reset();
            m_SceneView.reset();
        }

        m_CocHandle = bindless.Register(m_CocView);
        m_NearFillHandle = bindless.Register(m_NearFillView);
        m_FarFillHandle = bindless.Register(m_FarFillView);
        if (composited)
        {
            m_SceneHandle = bindless.Register(m_SceneView);
        }

        m_PrefilterSet =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SceneRenderer DoF Prefilter Set",
                                                 .Layout = m_PrefilterSetLayout,
                                             });
        m_PrefilterSet->Write(0, composited ? m_SceneView : hdrView);
        m_PrefilterSet->Write(1, depthView);
        m_PrefilterSet->Write(2, m_Sampler);
        m_PrefilterSet->Write(3, m_NearView);
        m_PrefilterSet->Write(4, m_FarView);
        m_PrefilterSet->Write(5, m_CocView);

        m_TileSet = DescriptorSet::Create(m_Context, {
                                                         .Name = "SceneRenderer DoF Tile Set",
                                                         .Layout = bloomDownUpLayout,
                                                     });
        m_TileSet->Write(0, m_CocView);
        m_TileSet->Write(1, m_Sampler);
        m_TileSet->Write(2, m_TileView);

        // Builds one gather set: the layer to gather, the shared tile records, and the destination.
        auto MakeGatherSet = [&](const char* name, const Ref<ImageView>& layer,
                                 const Ref<ImageView>& dest) -> Ref<DescriptorSet>
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = name, .Layout = m_GatherSetLayout});
            set->Write(0, layer);
            set->Write(1, m_TileView);
            set->Write(2, m_Sampler);
            set->Write(3, dest);
            return set;
        };
        m_NearGatherSet =
            MakeGatherSet("SceneRenderer DoF Near Gather Set", m_NearView, m_NearBlurView);
        m_FarGatherSet =
            MakeGatherSet("SceneRenderer DoF Far Gather Set", m_FarView, m_FarBlurView);

        // The fill sets ride the borrowed bloom down/up layout: sampled source, sampler, storage
        // destination — exactly a fill dispatch's shape.
        auto MakeFillSet = [&](const char* name, const Ref<ImageView>& source,
                               const Ref<ImageView>& dest) -> Ref<DescriptorSet>
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = name, .Layout = bloomDownUpLayout});
            set->Write(0, source);
            set->Write(1, m_Sampler);
            set->Write(2, dest);
            return set;
        };
        m_NearFillSet =
            MakeFillSet("SceneRenderer DoF Near Fill Set", m_NearBlurView, m_NearFillView);
        m_FarFillSet = MakeFillSet("SceneRenderer DoF Far Fill Set", m_FarBlurView, m_FarFillView);
    }

    void DofChain::Declare(RenderGraph& graph, const ResourceId sourceId, const ResourceId depthId,
                           const ResourceId nearId, const ResourceId farId, const ResourceId cocId,
                           const ResourceId tileId, const ResourceId nearBlurId,
                           const ResourceId farBlurId, const ResourceId nearFillId,
                           const ResourceId farFillId, const bool stagesOnly)
    {
        const uvec2 allocExtent = m_Extent;

        // Stage 1 — circle of confusion + prefilter: the layer split every later stage reads.
        {
            const Ref<ComputePipeline> pipeline = m_PrefilterPipeline;
            const Ref<DescriptorSet> set = m_PrefilterSet;
            graph.AddComputePass("DoF CoC Prefilter")
                .Sample(sourceId)
                .Sample(depthId)
                .StorageWrite(nearId)
                .StorageWrite(farId)
                .StorageWrite(cocId)
                .Execute(
                    [pipeline, set, allocExtent](PassContext& inner)
                    {
                        const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                        VE_ASSERT(viewPtr != nullptr, "DoF prefilter pass: null SceneView");
                        const SceneView& view = *viewPtr;

                        const uvec2 halfValid = DofHalfExtent(view.RenderExtent);
                        const SubRectUv full = ComputeSubRectUv(view.RenderExtent, allocExtent);
                        const mat4 proj = view.Camera.Projection();

                        // The circle of confusion is a pixel measure, so it follows the rendered
                        // sub-rect rather than the allocation: a frame at a reduced render scale
                        // has fewer pixels across the same sensor.
                        const f32 cocScale = view.DofCocScale *
                                             static_cast<f32>(view.RenderExtent.y) /
                                             static_cast<f32>(std::max(allocExtent.y, 1u));

                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1, // set 0 is reserved for the bindless registry
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(DofPrefilterPush{
                            .DestExtent = halfValid,
                            .ScaleUV = full.Scale,
                            .MaxUV = full.Max,
                            .DepthParams = vec2(proj[2][2], proj[3][2]),
                            .Aperture = view.DofAperture,
                            .FocusDistance = view.DofFocusDistance,
                            .CocScale = cocScale,
                            .MaxCoc = ClampDofMaxCoc(view.DofMaxCoc),
                        });
                        cmd.Dispatch((halfValid.x + 7) / 8, (halfValid.y + 7) / 8, 1);
                    });
        }

        // Stage 2 — tile reduction + dilation: the per-tile kernel bound the gather reads.
        {
            const Ref<ComputePipeline> pipeline = m_TilePipeline;
            const Ref<DescriptorSet> set = m_TileSet;
            graph.AddComputePass("DoF Tile Dilate")
                .Sample(cocId)
                .StorageWrite(tileId)
                .Execute(
                    [pipeline, set](PassContext& inner)
                    {
                        const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                        VE_ASSERT(viewPtr != nullptr, "DoF tile pass: null SceneView");
                        const uvec2 halfValid = DofHalfExtent(viewPtr->RenderExtent);
                        const uvec2 tiles = TileGridOf(halfValid);

                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(DofTilePush{
                            .DestExtent = tiles,
                            .SourceExtent = halfValid,
                        });
                        cmd.Dispatch((tiles.x + 7) / 8, (tiles.y + 7) / 8, 1);
                    });
        }

        // The debug arm stops here: it inspects the circle of confusion without touching the HDR
        // tail, so the gather, fill, and composite stay off.
        if (stagesOnly)
        {
            return;
        }

        // Stage 3 — ring gather, once per layer. The near layer spills over sharp geometry behind
        // it; the far layer is clamped against its destination inside the shader so it cannot.
        struct GatherStage
        {
            const char* Name;
            ResourceId Source;
            ResourceId Dest;
            Ref<DescriptorSet> Set;
            u32 NearField;
        };
        const std::array<GatherStage, 2> gathers{
            GatherStage{.Name = "DoF Gather Near",
                        .Source = nearId,
                        .Dest = nearBlurId,
                        .Set = m_NearGatherSet,
                        .NearField = 1},
            GatherStage{.Name = "DoF Gather Far",
                        .Source = farId,
                        .Dest = farBlurId,
                        .Set = m_FarGatherSet,
                        .NearField = 0},
        };
        for (const GatherStage& stage : gathers)
        {
            const Ref<ComputePipeline> pipeline = m_GatherPipeline;
            const Ref<DescriptorSet> set = stage.Set;
            const u32 nearField = stage.NearField;
            graph.AddComputePass(stage.Name)
                .Sample(stage.Source)
                .Sample(tileId)
                .StorageWrite(stage.Dest)
                .Execute(
                    [pipeline, set, nearField](PassContext& inner)
                    {
                        const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                        VE_ASSERT(viewPtr != nullptr, "DoF gather pass: null SceneView");
                        const SceneView& view = *viewPtr;
                        const uvec2 halfValid = DofHalfExtent(view.RenderExtent);

                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(DofGatherPush{
                            .DestExtent = halfValid,
                            .RingCount = ClampDofRingCount(view.DofRingCount),
                            .NearField = nearField,
                            .MaxCoc = ClampDofMaxCoc(view.DofMaxCoc),
                            .Pad0 = 0.0f,
                        });
                        cmd.Dispatch((halfValid.x + 7) / 8, (halfValid.y + 7) / 8, 1);
                    });
        }

        // Stage 4 — fill, once per layer: closes the gaps a fixed ring budget leaves at a large
        // circle of confusion.
        struct FillStage
        {
            const char* Name;
            ResourceId Source;
            ResourceId Dest;
            Ref<DescriptorSet> Set;
        };
        const std::array<FillStage, 2> fills{
            FillStage{.Name = "DoF Fill Near",
                      .Source = nearBlurId,
                      .Dest = nearFillId,
                      .Set = m_NearFillSet},
            FillStage{.Name = "DoF Fill Far",
                      .Source = farBlurId,
                      .Dest = farFillId,
                      .Set = m_FarFillSet},
        };
        for (const FillStage& stage : fills)
        {
            const Ref<ComputePipeline> pipeline = m_FillPipeline;
            const Ref<DescriptorSet> set = stage.Set;
            graph.AddComputePass(stage.Name)
                .Sample(stage.Source)
                .StorageWrite(stage.Dest)
                .Execute(
                    [pipeline, set](PassContext& inner)
                    {
                        const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                        VE_ASSERT(viewPtr != nullptr, "DoF fill pass: null SceneView");
                        const uvec2 halfValid = DofHalfExtent(viewPtr->RenderExtent);

                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(DofFillPush{.DestExtent = halfValid});
                        cmd.Dispatch((halfValid.x + 7) / 8, (halfValid.y + 7) / 8, 1);
                    });
        }
    }
}
