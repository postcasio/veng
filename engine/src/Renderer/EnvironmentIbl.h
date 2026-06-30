#pragma once

#include <Veng/Veng.h>

#include <array>
#include <vector>

namespace Veng
{
    class EnvironmentMap;
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;

    /// @brief Generates and owns the image-based-lighting maps derived from an EnvironmentMap.
    ///
    /// From an equirectangular HDR panorama it builds a radiance cubemap (the skybox source),
    /// a diffuse irradiance cubemap, a GGX-prefiltered specular cubemap (roughness mip chain),
    /// and the environment-independent BRDF integration LUT — all through compute, off the
    /// bindless registry, mirroring the bloom/hi-Z compute-with-manual-barriers pattern. The
    /// four sampled maps + a linear sampler are exposed as one descriptor set the deferred
    /// lighting pass binds (set 2). Generation is recorded once when the bound environment
    /// changes (the caller gates the call); the BRDF LUT is generated once on first use.
    class EnvironmentIbl
    {
    public:
        /// @brief Creates the IBL resources and generation pipelines (no generation recorded yet).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the core IBL compute shaders.
        /// @return A new EnvironmentIbl.
        static Unique<EnvironmentIbl> Create(Context& context, AssetManager& assets);

        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~EnvironmentIbl();

        EnvironmentIbl(const EnvironmentIbl&) = delete;
        EnvironmentIbl& operator=(const EnvironmentIbl&) = delete;

        /// @brief Records the BRDF LUT generation + transitions the cubes to a sampled layout, once.
        ///
        /// Idempotent: only the first call records anything. Leaves every consumer map in a
        /// shader-read layout so the lighting pass can bind the set even before an environment
        /// is set (the lighting shader gates the sample on the IblEnabled push flag).
        /// @param cmd The command buffer the init is recorded into.
        void EnsureInitialized(CommandBuffer& cmd);

        /// @brief Records the radiance/irradiance/prefilter generation from `environment`.
        ///
        /// @param cmd         The command buffer the generation is recorded into.
        /// @param environment The source panorama; its bindless handle samples the equirect.
        /// @pre EnsureInitialized has run (or runs in the same Execute before this).
        void Generate(CommandBuffer& cmd, const Veng::EnvironmentMap& environment);

        /// @brief The consumer descriptor-set layout the lighting pipeline reserves (set 2).
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const
        {
            return m_ConsumerSetLayout;
        }

        /// @brief The consumer descriptor set the lighting pass binds (radiance/irradiance/prefilter/BRDF + sampler).
        [[nodiscard]] const Ref<DescriptorSet>& GetSet() const { return m_ConsumerSet; }

        /// @brief Number of roughness mips in the prefiltered specular cube (the lighting LOD range).
        [[nodiscard]] u32 GetPrefilterMipCount() const;

    private:
        EnvironmentIbl(Context& context, AssetManager& assets);

        Context& m_Context;

        Ref<Image> m_RadianceImage;
        Ref<ImageView> m_RadianceCubeView;    // Cube, all mips — sampled
        Ref<ImageView> m_RadianceStorageView; // Array2D, 6 layers, mip 0 — written

        Ref<Image> m_IrradianceImage;
        Ref<ImageView> m_IrradianceCubeView;
        Ref<ImageView> m_IrradianceStorageView;

        Ref<Image> m_PrefilterImage;
        Ref<ImageView> m_PrefilterCubeView;                  // Cube, all mips — sampled
        std::vector<Ref<ImageView>> m_PrefilterStorageViews; // one Array2D view per mip — written

        Ref<Image> m_BrdfImage;
        Ref<ImageView> m_BrdfView;        // 2D — sampled
        Ref<ImageView> m_BrdfStorageView; // 2D — written

        Ref<Sampler> m_Sampler;

        // Generation pipelines + their per-pass descriptor sets (off bindless, set 1).
        Ref<DescriptorSetLayout> m_EquirectSetLayout;
        Ref<PipelineLayout> m_EquirectLayout;
        Ref<ComputePipeline> m_EquirectPipeline;
        Ref<DescriptorSet> m_EquirectSet;

        Ref<DescriptorSetLayout> m_ConvolveSetLayout; // shared by irradiance + prefilter
        Ref<PipelineLayout> m_IrradianceLayout;
        Ref<ComputePipeline> m_IrradiancePipeline;
        Ref<DescriptorSet> m_IrradianceSet;

        Ref<PipelineLayout> m_PrefilterLayout;
        Ref<ComputePipeline> m_PrefilterPipeline;
        std::vector<Ref<DescriptorSet>> m_PrefilterSets; // one per mip

        Ref<DescriptorSetLayout> m_BrdfSetLayout;
        Ref<PipelineLayout> m_BrdfLayout;
        Ref<ComputePipeline> m_BrdfPipeline;
        Ref<DescriptorSet> m_BrdfSet;

        // The set the deferred lighting pass binds.
        Ref<DescriptorSetLayout> m_ConsumerSetLayout;
        Ref<DescriptorSet> m_ConsumerSet;

        bool m_Initialized = false;
    };
}
