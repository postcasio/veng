#pragma once

#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Veng.h>

#include <array>
#include <span>
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
        /// Runs the equirectangular panorama into the owned radiance cube, then convolves that
        /// cube through GenerateFromCube — so the only equirect-specific work is the entry stage.
        /// @param cmd         The command buffer the generation is recorded into.
        /// @param environment The source panorama; its bindless handle samples the equirect.
        /// @pre EnsureInitialized has run (or runs in the same Execute before this).
        void Generate(CommandBuffer& cmd, const Veng::EnvironmentMap& environment);

        /// @brief Records the irradiance + prefilter convolution from a supplied radiance cube.
        ///
        /// The source-agnostic convolution arm: point the irradiance and prefilter passes at
        /// `radianceCube` and run them, filling the owned irradiance/prefilter maps + the consumer
        /// set the lighting pass binds. Leaves the BRDF LUT untouched (environment-independent,
        /// generated once by EnsureInitialized). The supplied cube is read, never copied; its face
        /// resolution feeds the prefilter mip integration.
        /// @param cmd          The command buffer the convolution is recorded into.
        /// @param radianceCube A cube-view of the source radiance, in a sampled layout.
        /// @param sourceFaceSize Edge length in texels of `radianceCube`'s faces.
        /// @pre EnsureInitialized has run (or runs in the same Execute before this), and
        ///      `radianceCube` is resident + shader-readable when this records.
        void GenerateFromCube(CommandBuffer& cmd, const Ref<ImageView>& radianceCube,
                              u32 sourceFaceSize);

        /// @brief Projects a downloaded radiance cube into an irradiance spherical-harmonic set.
        ///
        /// The cheap ambient arm's cube source: samples every texel of a host-side RGBA16F cube
        /// snapshot (six faces, layer-major, each face `faceSize`² texels), accumulates the
        /// order-2 radiance SH weighted by each texel's solid angle, and convolves it with the
        /// Lambertian cosine lobe — the same order-2, 9-coefficient irradiance set the SH skylight
        /// arm evaluates. Device-free (no GPU work), so the caller downloads the cube once on its
        /// dirty signal and projects here.
        /// @param cubeTexels A tightly-packed RGBA16F cube snapshot, six faces layer-major.
        /// @param faceSize   The cube's face edge length in texels.
        /// @return The cosine-convolved irradiance SH set.
        /// @pre `cubeTexels.size()` is `faceSize * faceSize * 6 * 8` (RGBA16F, six faces).
        [[nodiscard]] static Sh9 ProjectCubeToIrradianceSh(std::span<const u8> cubeTexels,
                                                           u32 faceSize);

        /// @brief Projects an environment panorama into an irradiance skylight SH set.
        ///
        /// The cheap ambient arm's environment source: renders the equirectangular panorama into
        /// the owned radiance cube through a self-contained immediate submit, downloads it, and
        /// projects it via ProjectCubeToIrradianceSh — so display (the skybox) and the SH ambient
        /// read the one cube. Blocks (immediate submit + readback), bounded by the caller's
        /// once-per-environment-change gate. Overwrites the radiance cube in place, so the skybox
        /// still samples the current environment.
        /// @param environment The source panorama; its bindless handle samples the equirect.
        /// @return The cosine-convolved irradiance SH set for the environment's sky.
        [[nodiscard]] Sh9 ProjectEnvironmentToIrradianceSh(const Veng::EnvironmentMap& environment);

        /// @brief The consumer descriptor-set layout the lighting pipeline reserves (set 2).
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const
        {
            return m_ConsumerSetLayout;
        }

        /// @brief The consumer descriptor set the lighting pass binds (radiance/irradiance/prefilter/BRDF + sampler).
        [[nodiscard]] const Ref<DescriptorSet>& GetSet() const { return m_ConsumerSet; }

        /// @brief Number of roughness mips in the prefiltered specular cube (the lighting LOD range).
        [[nodiscard]] u32 GetPrefilterMipCount() const;

        /// @brief The owned radiance cube image, exposed for tests reading back the convolution source.
        [[nodiscard]] const Ref<Image>& GetRadianceImage() const { return m_RadianceImage; }

        /// @brief The irradiance cube image, exposed for tests reading back the convolved diffuse map.
        [[nodiscard]] const Ref<Image>& GetIrradianceImage() const { return m_IrradianceImage; }

        /// @brief The irradiance cube's face edge length in texels, exposed for tests.
        [[nodiscard]] static u32 GetIrradianceFaceSize();

    private:
        EnvironmentIbl(Context& context, AssetManager& assets);

        /// @brief Records the equirectangular panorama into the owned radiance cube.
        void RecordEquirectToCube(CommandBuffer& cmd, const Veng::EnvironmentMap& environment);

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
