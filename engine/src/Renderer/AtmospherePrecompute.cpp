#include "AtmospherePrecompute.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
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
        // Core atmosphere compute shaders (engine/assets/core/shaders/atmosphere/*.comp.slang).
        constexpr AssetId TransmittanceCompId{0x60BF176C73AC8D8EULL};
        constexpr AssetId SingleScatteringCompId{0xF71F0FF833C46806ULL};
        constexpr AssetId MultipleScatteringCompId{0xCF5261F9B3701C37ULL};
        constexpr AssetId IrradianceCompId{0xE28A90B88BF4ED70ULL};

        // LUT dimensions; mirror the static sizes in atmosphere_common.slang.
        constexpr u32 TransmittanceWidth = 256;
        constexpr u32 TransmittanceHeight = 64;
        constexpr u32 ScatteringMuS = 32;
        constexpr u32 ScatteringMu = 128;
        constexpr u32 ScatteringR = 32;
        constexpr u32 IrradianceWidth = 64;
        constexpr u32 IrradianceHeight = 16;

        // The multiple-scattering iteration count beyond single scattering. An even count
        // leaves the running total back in volume A (single scattering seeds A).
        constexpr u32 MultipleScatteringOrders = 4;

        constexpr u32 Groups2D(u32 extent)
        {
            return (extent + 7) / 8;
        }
        constexpr u32 Groups3D(u32 extent)
        {
            return (extent + 3) / 4;
        }

        // Mirrors AtmosphereParams in atmosphere_common.slang field-for-field, including the
        // explicit scalar pads that align the trailing float3 to a 16-byte boundary.
        struct AtmospherePush
        {
            vec3 RayleighScattering;
            f32 RayleighHeight;
            vec3 MieScattering;
            f32 MieExtinction;
            vec3 OzoneAbsorption;
            f32 MieHeight;
            f32 MieAnisotropy;
            f32 OzoneCenter;
            f32 OzoneWidth;
            f32 PlanetRadius;
            f32 AtmosphereRadius;
            f32 SunAngularRadius;
            f32 Pad0;
            f32 Pad1;
            vec3 SunIrradiance;
            f32 Pad2;
        };

        AtmospherePush MakePush(const Atmosphere& a)
        {
            return AtmospherePush{
                .RayleighScattering = a.RayleighScattering,
                .RayleighHeight = a.RayleighHeight,
                .MieScattering = a.MieScattering,
                .MieExtinction = a.MieExtinction,
                .OzoneAbsorption = a.OzoneAbsorption,
                .MieHeight = a.MieHeight,
                .MieAnisotropy = a.MieAnisotropy,
                .OzoneCenter = a.OzoneCenter,
                .OzoneWidth = a.OzoneWidth,
                .PlanetRadius = a.PlanetRadius,
                .AtmosphereRadius = a.AtmosphereRadius,
                .SunAngularRadius = a.SunAngularRadius,
                .SunIrradiance = a.SunIrradiance,
            };
        }
    }

    Unique<AtmospherePrecompute> AtmospherePrecompute::Create(Context& context,
                                                              AssetManager& assets)
    {
        return Unique<AtmospherePrecompute>(new AtmospherePrecompute(context, assets));
    }

    AtmospherePrecompute::AtmospherePrecompute(Context& context, AssetManager& assets)
        : m_Context(context)
    {
        auto LoadShader = [&assets](const AssetId id, const char* what) -> Ref<ShaderModule>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "AtmospherePrecompute: {} shader load failed: {}", what,
                      result.error().Detail);
            return result->Get()->Module;
        };

        // Transmittance LUT (2D).
        m_TransmittanceImage =
            Image::Create(m_Context, {
                                         .Name = "Atmosphere Transmittance",
                                         .Extent = {TransmittanceWidth, TransmittanceHeight, 1},
                                         .Format = Format::RGBA16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_TransmittanceView = ImageView::Create(
            m_Context, {.Name = "Atmosphere Transmittance View", .Image = m_TransmittanceImage});
        m_TransmittanceStorageView =
            ImageView::Create(m_Context, {.Name = "Atmosphere Transmittance Storage View",
                                          .Image = m_TransmittanceImage});

        // Scattering volumes (Type3D), ping-ponged by the multiple-scattering iteration.
        auto MakeScattering = [this](const char* name) -> Ref<Image>
        {
            return Image::Create(m_Context,
                                 {
                                     .Name = name,
                                     .Extent = {ScatteringMuS, ScatteringMu, ScatteringR},
                                     .Format = Format::RGBA16Sfloat,
                                     .Type = ImageType::Type3D,
                                     .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                 });
        };
        m_ScatteringImageA = MakeScattering("Atmosphere Scattering A");
        m_ScatteringViewA = ImageView::Create(m_Context, {.Name = "Atmosphere Scattering A View",
                                                          .Image = m_ScatteringImageA,
                                                          .ViewType = ImageViewType::Type3D});
        m_ScatteringStorageViewA =
            ImageView::Create(m_Context, {.Name = "Atmosphere Scattering A Storage View",
                                          .Image = m_ScatteringImageA,
                                          .ViewType = ImageViewType::Type3D});
        m_ScatteringImageB = MakeScattering("Atmosphere Scattering B");
        m_ScatteringViewB = ImageView::Create(m_Context, {.Name = "Atmosphere Scattering B View",
                                                          .Image = m_ScatteringImageB,
                                                          .ViewType = ImageViewType::Type3D});
        m_ScatteringStorageViewB =
            ImageView::Create(m_Context, {.Name = "Atmosphere Scattering B Storage View",
                                          .Image = m_ScatteringImageB,
                                          .ViewType = ImageViewType::Type3D});

        // Ground-irradiance LUT (2D).
        m_IrradianceImage =
            Image::Create(m_Context, {
                                         .Name = "Atmosphere Irradiance",
                                         .Extent = {IrradianceWidth, IrradianceHeight, 1},
                                         .Format = Format::RGBA16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_IrradianceView = ImageView::Create(
            m_Context, {.Name = "Atmosphere Irradiance View", .Image = m_IrradianceImage});
        m_IrradianceStorageView = ImageView::Create(
            m_Context, {.Name = "Atmosphere Irradiance Storage View", .Image = m_IrradianceImage});

        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "Atmosphere Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Linear,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                               });

        // Transmittance generation: a single 2D storage destination on set 1.
        m_TransmittanceSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Atmosphere Transmittance Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageImage,
                                         .Count = 1,
                                         .Stages = ShaderStage::Compute}},
                       });
        m_TransmittanceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Atmosphere Transmittance Layout",
                .DescriptorSetLayouts = {m_TransmittanceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<AtmospherePush>(ShaderStage::Compute)},
            });
        m_TransmittancePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "Atmosphere Transmittance Pipeline",
                .PipelineLayout = m_TransmittanceLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute,
                                .Module = LoadShader(TransmittanceCompId, "transmittance")},
            });
        m_TransmittanceSet =
            DescriptorSet::Create(m_Context, {.Name = "Atmosphere Transmittance Set",
                                              .Layout = m_TransmittanceSetLayout});
        m_TransmittanceSet->Write(0, m_TransmittanceStorageView);

        // Single-scattering: transmittance (sampled) + sampler + scattering destination.
        m_SingleSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Atmosphere Single Set Layout",
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
        m_SingleLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Atmosphere Single Layout",
                .DescriptorSetLayouts = {m_SingleSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<AtmospherePush>(ShaderStage::Compute)},
            });
        m_SinglePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "Atmosphere Single Pipeline",
                .PipelineLayout = m_SingleLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute,
                                .Module = LoadShader(SingleScatteringCompId, "single scattering")},
            });
        m_SingleSet = DescriptorSet::Create(
            m_Context, {.Name = "Atmosphere Single Set", .Layout = m_SingleSetLayout});
        m_SingleSet->Write(0, m_TransmittanceView);
        m_SingleSet->Write(1, m_Sampler);
        m_SingleSet->Write(2, m_ScatteringStorageViewA);

        // Multiple-scattering: prev total (sampled) + transmittance (sampled) + sampler + next
        // total destination. Two sets toggle the ping-pong source/destination.
        m_MultiSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Atmosphere Multi Set Layout",
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
        m_MultiLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Atmosphere Multi Layout",
                .DescriptorSetLayouts = {m_MultiSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<AtmospherePush>(ShaderStage::Compute)},
            });
        m_MultiPipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "Atmosphere Multi Pipeline",
                           .PipelineLayout = m_MultiLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(MultipleScatteringCompId,
                                                                "multiple scattering")},
                       });
        m_MultiSetAtoB = DescriptorSet::Create(
            m_Context, {.Name = "Atmosphere Multi A->B Set", .Layout = m_MultiSetLayout});
        m_MultiSetAtoB->Write(0, m_ScatteringViewA);
        m_MultiSetAtoB->Write(1, m_TransmittanceView);
        m_MultiSetAtoB->Write(2, m_Sampler);
        m_MultiSetAtoB->Write(3, m_ScatteringStorageViewB);
        m_MultiSetBtoA = DescriptorSet::Create(
            m_Context, {.Name = "Atmosphere Multi B->A Set", .Layout = m_MultiSetLayout});
        m_MultiSetBtoA->Write(0, m_ScatteringViewB);
        m_MultiSetBtoA->Write(1, m_TransmittanceView);
        m_MultiSetBtoA->Write(2, m_Sampler);
        m_MultiSetBtoA->Write(3, m_ScatteringStorageViewA);

        // Irradiance: scattering total (sampled) + sampler + 2D destination. The source view is
        // rebound per Generate to whichever volume holds the total; seed it to A.
        m_IrradianceSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Atmosphere Irradiance Set Layout",
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
        m_IrradianceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Atmosphere Irradiance Layout",
                .DescriptorSetLayouts = {m_IrradianceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<AtmospherePush>(ShaderStage::Compute)},
            });
        m_IrradiancePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "Atmosphere Irradiance Pipeline",
                           .PipelineLayout = m_IrradianceLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = LoadShader(IrradianceCompId, "irradiance")},
                       });
        m_IrradianceSet = DescriptorSet::Create(
            m_Context, {.Name = "Atmosphere Irradiance Set", .Layout = m_IrradianceSetLayout});
        m_IrradianceSet->Write(0, m_ScatteringViewA);
        m_IrradianceSet->Write(1, m_Sampler);
        m_IrradianceSet->Write(2, m_IrradianceStorageView);

        // The set the sky pass binds (set 1): scattering total, transmittance, linear sampler.
        // The scattering slot is rebound per Generate to whichever volume holds the total.
        m_ConsumerSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Atmosphere Consumer Set Layout",
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
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        m_ConsumerSet = DescriptorSet::Create(
            m_Context, {.Name = "Atmosphere Consumer Set", .Layout = m_ConsumerSetLayout});
        m_ConsumerSet->Write(0, m_ScatteringViewA);
        m_ConsumerSet->Write(1, m_TransmittanceView);
        m_ConsumerSet->Write(2, m_Sampler);
    }

    AtmospherePrecompute::~AtmospherePrecompute() = default;

    void AtmospherePrecompute::EnsureInitialized(CommandBuffer& cmd)
    {
        if (m_Initialized)
        {
            return;
        }
        m_Initialized = true;

        // Transition every table to a sampled layout so the sky pass can bind the consumer set
        // before any atmosphere is generated (the shader gates the sample on the Enabled flag, so
        // the as-yet-uninitialized contents are never read).
        cmd.PrepareForAccess(m_TransmittanceView, AccessKind::Sample);
        cmd.PrepareForAccess(m_ScatteringViewA, AccessKind::Sample);
        cmd.PrepareForAccess(m_ScatteringViewB, AccessKind::Sample);
        cmd.PrepareForAccess(m_IrradianceView, AccessKind::Sample);
    }

    void AtmospherePrecompute::Generate(CommandBuffer& cmd, const Atmosphere& atmosphere)
    {
        const AtmospherePush push = MakePush(atmosphere);

        // Transmittance LUT.
        cmd.PrepareForAccess(m_TransmittanceStorageView, AccessKind::StorageWrite);
        cmd.BindPipeline(m_TransmittancePipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_TransmittanceSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(push);
        cmd.Dispatch(Groups2D(TransmittanceWidth), Groups2D(TransmittanceHeight), 1);
        cmd.PrepareForAccess(m_TransmittanceView, AccessKind::Sample);

        // Single scattering seeds the running total into volume A.
        cmd.PrepareForAccess(m_ScatteringStorageViewA, AccessKind::StorageWrite);
        cmd.BindPipeline(m_SinglePipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_SingleSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(push);
        cmd.Dispatch(Groups3D(ScatteringMuS), Groups3D(ScatteringMu), Groups3D(ScatteringR));
        cmd.PrepareForAccess(m_ScatteringViewA, AccessKind::Sample);

        // Multiple-scattering iteration: ping-pong A<->B, each pass reading the running total and
        // writing the next total into the other (write-only) volume.
        cmd.BindPipeline(m_MultiPipeline);
        bool totalInA = true;
        for (u32 order = 0; order < MultipleScatteringOrders; ++order)
        {
            const Ref<ImageView>& destStorage =
                totalInA ? m_ScatteringStorageViewB : m_ScatteringStorageViewA;
            const Ref<ImageView>& destSampled = totalInA ? m_ScatteringViewB : m_ScatteringViewA;
            const Ref<DescriptorSet>& set = totalInA ? m_MultiSetAtoB : m_MultiSetBtoA;

            cmd.PrepareForAccess(destStorage, AccessKind::StorageWrite);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {set},
                .FirstSet = 1,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(push);
            cmd.Dispatch(Groups3D(ScatteringMuS), Groups3D(ScatteringMu), Groups3D(ScatteringR));
            cmd.PrepareForAccess(destSampled, AccessKind::Sample);
            totalInA = !totalInA;
        }
        m_TotalInA = totalInA;

        // Rebind the consumer + irradiance scattering slots to whichever volume holds the total.
        const Ref<ImageView>& totalView = m_TotalInA ? m_ScatteringViewA : m_ScatteringViewB;
        m_ConsumerSet->Write(0, totalView);
        m_IrradianceSet->Write(0, totalView);

        // Ground irradiance from the scattering total.
        cmd.PrepareForAccess(m_IrradianceStorageView, AccessKind::StorageWrite);
        cmd.BindPipeline(m_IrradiancePipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_IrradianceSet},
            .FirstSet = 1,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(push);
        cmd.Dispatch(Groups2D(IrradianceWidth), Groups2D(IrradianceHeight), 1);
        cmd.PrepareForAccess(m_IrradianceView, AccessKind::Sample);
    }
}
