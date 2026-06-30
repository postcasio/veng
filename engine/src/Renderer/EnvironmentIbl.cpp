#include "EnvironmentIbl.h"

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    namespace
    {
        // Core IBL compute shaders (engine/assets/core/shaders/ibl_*.comp.slang).
        constexpr AssetId EquirectToCubeCompId{0xD783ACB75C22D848ULL};
        constexpr AssetId IrradianceCompId{0x742B70980583A3DFULL};
        constexpr AssetId PrefilterCompId{0x05EF9BB89AFFB151ULL};
        constexpr AssetId BrdfLutCompId{0x8A00744B8956EE08ULL};

        // Generated-map sizes. The radiance cube backs the skybox + prefilter; irradiance is a
        // low-res diffuse convolution; the prefilter chain's mips are roughness levels.
        constexpr u32 RadianceCubeSize = 512;
        constexpr u32 IrradianceSize = 32;
        constexpr u32 PrefilterBaseSize = 128;
        constexpr u32 PrefilterMips = 5;
        constexpr u32 BrdfLutSize = 256;
        constexpr u32 PrefilterSamples = 128;
        constexpr u32 BrdfSamples = 1024;

        constexpr u32 CubeFaces = 6;

        constexpr u32 Groups(u32 extent)
        {
            return (extent + 7) / 8;
        }

        struct EquirectPush
        {
            u32 Equirect;
            u32 Sampler;
            u32 FaceSize;
            u32 Pad0;
        };

        struct IrradiancePush
        {
            u32 FaceSize;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        struct PrefilterPush
        {
            u32 FaceSize;
            f32 Roughness;
            u32 SampleCount;
            f32 SourceSize;
        };

        struct BrdfPush
        {
            u32 Size;
            u32 SampleCount;
            u32 Pad0;
            u32 Pad1;
        };
    }

    Unique<EnvironmentIbl> EnvironmentIbl::Create(Context& context, AssetManager& assets)
    {
        return Unique<EnvironmentIbl>(new EnvironmentIbl(context, assets));
    }

    EnvironmentIbl::EnvironmentIbl(Context& context, AssetManager& assets) : m_Context(context)
    {
        auto LoadShader = [&assets](const AssetId id, const char* what) -> Ref<ShaderModule>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "EnvironmentIbl: {} shader load failed: {}", what,
                      result.error().Detail);
            return result->Get()->Module;
        };

        // Radiance cube (skybox source + prefilter source): six layers, single mip.
        m_RadianceImage =
            Image::Create(m_Context, {
                                         .Name = "IBL Radiance Cube",
                                         .Extent = {RadianceCubeSize, RadianceCubeSize, 1},
                                         .Layers = CubeFaces,
                                         .Format = Format::RGBA16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_RadianceCubeView = ImageView::Create(m_Context, {
                                                              .Name = "IBL Radiance Cube View",
                                                              .Image = m_RadianceImage,
                                                              .ViewType = ImageViewType::Cube,
                                                              .ArrayLayers = CubeFaces,
                                                          });
        m_RadianceStorageView =
            ImageView::Create(m_Context, {
                                             .Name = "IBL Radiance Storage View",
                                             .Image = m_RadianceImage,
                                             .ViewType = ImageViewType::Array2D,
                                             .ArrayLayers = CubeFaces,
                                         });

        // Irradiance cube (diffuse): tiny, single mip.
        m_IrradianceImage =
            Image::Create(m_Context, {
                                         .Name = "IBL Irradiance Cube",
                                         .Extent = {IrradianceSize, IrradianceSize, 1},
                                         .Layers = CubeFaces,
                                         .Format = Format::RGBA16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_IrradianceCubeView = ImageView::Create(m_Context, {
                                                                .Name = "IBL Irradiance Cube View",
                                                                .Image = m_IrradianceImage,
                                                                .ViewType = ImageViewType::Cube,
                                                                .ArrayLayers = CubeFaces,
                                                            });
        m_IrradianceStorageView =
            ImageView::Create(m_Context, {
                                             .Name = "IBL Irradiance Storage View",
                                             .Image = m_IrradianceImage,
                                             .ViewType = ImageViewType::Array2D,
                                             .ArrayLayers = CubeFaces,
                                         });

        // Prefiltered specular cube (roughness mip chain).
        m_PrefilterImage =
            Image::Create(m_Context, {
                                         .Name = "IBL Prefilter Cube",
                                         .Extent = {PrefilterBaseSize, PrefilterBaseSize, 1},
                                         .MipLevels = PrefilterMips,
                                         .Layers = CubeFaces,
                                         .Format = Format::RGBA16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_PrefilterCubeView = ImageView::Create(m_Context, {
                                                               .Name = "IBL Prefilter Cube View",
                                                               .Image = m_PrefilterImage,
                                                               .ViewType = ImageViewType::Cube,
                                                               .MipLevels = PrefilterMips,
                                                               .ArrayLayers = CubeFaces,
                                                           });
        m_PrefilterStorageViews.reserve(PrefilterMips);
        for (u32 mip = 0; mip < PrefilterMips; ++mip)
        {
            m_PrefilterStorageViews.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("IBL Prefilter Storage Mip {}", mip),
                               .Image = m_PrefilterImage,
                               .ViewType = ImageViewType::Array2D,
                               .BaseMipLevel = mip,
                               .MipLevels = 1,
                               .ArrayLayers = CubeFaces,
                           }));
        }

        // BRDF integration LUT (environment-independent).
        m_BrdfImage =
            Image::Create(m_Context, {
                                         .Name = "IBL BRDF LUT",
                                         .Extent = {BrdfLutSize, BrdfLutSize, 1},
                                         .Format = Format::RG16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_BrdfView = ImageView::Create(m_Context, {.Name = "IBL BRDF View", .Image = m_BrdfImage});
        m_BrdfStorageView =
            ImageView::Create(m_Context, {.Name = "IBL BRDF Storage View", .Image = m_BrdfImage});

        // Linear sampler covering the prefilter mips (roughness → LOD).
        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "IBL Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Linear,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                                   .MaxLod = static_cast<f32>(PrefilterMips),
                                               });

        // Generation pipelines. The equirect->cube pass samples the panorama through set-0
        // bindless and writes its own storage destination on set 1; the convolution passes
        // sample the radiance cube + a linear sampler and write storage, all on set 1.
        m_EquirectSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "IBL Equirect Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageImage,
                                         .Count = 1,
                                         .Stages = ShaderStage::Compute}},
                       });
        m_ConvolveSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "IBL Convolve Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_BrdfSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "IBL BRDF Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageImage,
                                         .Count = 1,
                                         .Stages = ShaderStage::Compute}},
                       });

        m_EquirectLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "IBL Equirect Layout",
                .DescriptorSetLayouts = {m_EquirectSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<EquirectPush>(ShaderStage::Compute)},
            });
        m_IrradianceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "IBL Irradiance Layout",
                .DescriptorSetLayouts = {m_ConvolveSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<IrradiancePush>(ShaderStage::Compute)},
            });
        m_PrefilterLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "IBL Prefilter Layout",
                .DescriptorSetLayouts = {m_ConvolveSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<PrefilterPush>(ShaderStage::Compute)},
            });
        m_BrdfLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "IBL BRDF Layout",
                .DescriptorSetLayouts = {m_BrdfSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<BrdfPush>(ShaderStage::Compute)},
            });

        m_EquirectPipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "IBL Equirect Pipeline",
                           .PipelineLayout = m_EquirectLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(EquirectToCubeCompId, "equirect")},
                       });
        m_IrradiancePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "IBL Irradiance Pipeline",
                           .PipelineLayout = m_IrradianceLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(IrradianceCompId, "irradiance")},
                       });
        m_PrefilterPipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "IBL Prefilter Pipeline",
                           .PipelineLayout = m_PrefilterLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(PrefilterCompId, "prefilter")},
                       });
        m_BrdfPipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "IBL BRDF Pipeline",
                           .PipelineLayout = m_BrdfLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(BrdfLutCompId, "BRDF LUT")},
                       });

        // Generation descriptor sets (the destination views are stable across generations).
        m_EquirectSet = DescriptorSet::Create(
            m_Context, {.Name = "IBL Equirect Set", .Layout = m_EquirectSetLayout});
        m_EquirectSet->Write(0, m_RadianceStorageView);

        m_IrradianceSet = DescriptorSet::Create(
            m_Context, {.Name = "IBL Irradiance Set", .Layout = m_ConvolveSetLayout});
        m_IrradianceSet->Write(0, m_RadianceCubeView);
        m_IrradianceSet->Write(1, m_Sampler);
        m_IrradianceSet->Write(2, m_IrradianceStorageView);

        m_PrefilterSets.reserve(PrefilterMips);
        for (u32 mip = 0; mip < PrefilterMips; ++mip)
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = fmt::format("IBL Prefilter Set {}", mip),
                                                  .Layout = m_ConvolveSetLayout});
            set->Write(0, m_RadianceCubeView);
            set->Write(1, m_Sampler);
            set->Write(2, m_PrefilterStorageViews[mip]);
            m_PrefilterSets.push_back(std::move(set));
        }

        m_BrdfSet =
            DescriptorSet::Create(m_Context, {.Name = "IBL BRDF Set", .Layout = m_BrdfSetLayout});
        m_BrdfSet->Write(0, m_BrdfStorageView);

        // The consumer set the lighting pass binds (set 2): radiance/irradiance/prefilter cubes,
        // the BRDF LUT, and the linear sampler.
        m_ConsumerSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "IBL Consumer Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 1,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 2,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 3,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 4,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        m_ConsumerSet = DescriptorSet::Create(
            m_Context, {.Name = "IBL Consumer Set", .Layout = m_ConsumerSetLayout});
        m_ConsumerSet->Write(0, m_RadianceCubeView);
        m_ConsumerSet->Write(1, m_IrradianceCubeView);
        m_ConsumerSet->Write(2, m_PrefilterCubeView);
        m_ConsumerSet->Write(3, m_BrdfView);
        m_ConsumerSet->Write(4, m_Sampler);
    }

    EnvironmentIbl::~EnvironmentIbl() = default;

    u32 EnvironmentIbl::GetPrefilterMipCount() const
    {
        return PrefilterMips;
    }

    void EnvironmentIbl::EnsureInitialized(CommandBuffer& cmd)
    {
        if (m_Initialized)
        {
            return;
        }
        m_Initialized = true;

        // BRDF LUT: environment-independent, generated once.
        cmd.PrepareForAccess(m_BrdfStorageView, AccessKind::StorageWrite);
        cmd.BindPipeline(m_BrdfPipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_BrdfSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(BrdfPush{.Size = BrdfLutSize, .SampleCount = BrdfSamples});
        cmd.Dispatch(Groups(BrdfLutSize), Groups(BrdfLutSize), 1);
        cmd.PrepareForAccess(m_BrdfView, AccessKind::Sample);

        // Transition the still-ungenerated cubes to a sampled layout so the lighting pass can
        // bind the consumer set before an environment is set (the shader gates the sample on
        // the IblEnabled push flag, so the as-yet-uninitialized contents are never read).
        cmd.PrepareForAccess(m_RadianceCubeView, AccessKind::Sample);
        cmd.PrepareForAccess(m_IrradianceCubeView, AccessKind::Sample);
        cmd.PrepareForAccess(m_PrefilterCubeView, AccessKind::Sample);
    }

    void EnvironmentIbl::Generate(CommandBuffer& cmd, const Veng::EnvironmentMap& environment)
    {
        // Equirect panorama -> radiance cube. The panorama is sampled through set-0 bindless.
        m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
        cmd.PrepareForAccess(m_RadianceStorageView, AccessKind::StorageWrite);
        cmd.BindPipeline(m_EquirectPipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_EquirectSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(EquirectPush{
            .Equirect = environment.GetHandle().Index,
            .Sampler = environment.GetSamplerHandle().Index,
            .FaceSize = RadianceCubeSize,
        });
        cmd.Dispatch(Groups(RadianceCubeSize), Groups(RadianceCubeSize), CubeFaces);
        cmd.PrepareForAccess(m_RadianceCubeView, AccessKind::Sample);

        // Radiance cube -> irradiance cube (diffuse convolution).
        cmd.PrepareForAccess(m_IrradianceStorageView, AccessKind::StorageWrite);
        cmd.BindPipeline(m_IrradiancePipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_IrradianceSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(IrradiancePush{.FaceSize = IrradianceSize});
        cmd.Dispatch(Groups(IrradianceSize), Groups(IrradianceSize), CubeFaces);
        cmd.PrepareForAccess(m_IrradianceCubeView, AccessKind::Sample);

        // Radiance cube -> prefiltered specular cube, one dispatch per roughness mip.
        cmd.BindPipeline(m_PrefilterPipeline);
        for (u32 mip = 0; mip < PrefilterMips; ++mip)
        {
            const u32 faceSize = std::max(1u, PrefilterBaseSize >> mip);
            const f32 roughness = PrefilterMips > 1
                                      ? static_cast<f32>(mip) / static_cast<f32>(PrefilterMips - 1)
                                      : 0.0f;
            cmd.PrepareForAccess(m_PrefilterStorageViews[mip], AccessKind::StorageWrite);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {m_PrefilterSets[mip]},
                .FirstSet = 1,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(PrefilterPush{
                .FaceSize = faceSize,
                .Roughness = roughness,
                .SampleCount = PrefilterSamples,
                .SourceSize = static_cast<f32>(RadianceCubeSize),
            });
            cmd.Dispatch(Groups(faceSize), Groups(faceSize), CubeFaces);
        }
        cmd.PrepareForAccess(m_PrefilterCubeView, AccessKind::Sample);
    }
}
