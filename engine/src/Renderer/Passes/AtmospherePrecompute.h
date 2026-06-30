#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Atmosphere.h>

#include <vector>

namespace Veng
{
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

    /// @brief Generates and owns the Bruneton precomputed-atmosphere lookup tables.
    ///
    /// From an Atmosphere parameter set it builds a transmittance LUT (2D), a single +
    /// multiple-scattering table packed into a Type3D volume, and a ground-irradiance LUT
    /// (2D) — all through compute, off the bindless registry, mirroring the IBL/bloom
    /// compute-with-manual-barriers pattern. The multiple-scattering iteration runs a small
    /// fixed set of dispatches, ping-ponging two write-only 3D volumes (never an in-place
    /// read-write). The scattering total + transmittance + a linear sampler are exposed as
    /// one descriptor set the sky pass binds (set 1). Generation is recorded once per
    /// Atmosphere change; the caller gates the call against its own last-atmosphere cache.
    class AtmospherePrecompute
    {
    public:
        /// @brief Creates the LUT resources and generation pipelines (no generation recorded yet).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the core atmosphere compute shaders.
        /// @return A new AtmospherePrecompute.
        static Unique<AtmospherePrecompute> Create(Context& context, AssetManager& assets);

        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~AtmospherePrecompute();

        AtmospherePrecompute(const AtmospherePrecompute&) = delete;
        AtmospherePrecompute& operator=(const AtmospherePrecompute&) = delete;

        /// @brief Transitions every LUT to a sampled layout, once, before any generation.
        ///
        /// Idempotent: only the first call records anything. Leaves the tables in a
        /// shader-read layout so the sky pass can bind the set even before an atmosphere is
        /// generated (the sky shader gates the sample on the Enabled push flag).
        /// @param cmd The command buffer the init is recorded into.
        void EnsureInitialized(CommandBuffer& cmd);

        /// @brief Records the transmittance, scattering, and irradiance generation from `atmosphere`.
        ///
        /// @param cmd        The command buffer the generation is recorded into.
        /// @param atmosphere The atmosphere parameters the tables are built for.
        /// @pre EnsureInitialized has run (or runs in the same Execute before this).
        void Generate(CommandBuffer& cmd, const Atmosphere& atmosphere);

        /// @brief The consumer descriptor-set layout the sky pipeline reserves (set 1).
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const
        {
            return m_ConsumerSetLayout;
        }

        /// @brief The consumer descriptor set the sky pass binds (scattering + transmittance + sampler).
        [[nodiscard]] const Ref<DescriptorSet>& GetSet() const { return m_ConsumerSet; }

    private:
        AtmospherePrecompute(Context& context, AssetManager& assets);

        Context& m_Context;

        Ref<Image> m_TransmittanceImage;
        Ref<ImageView> m_TransmittanceView;        // 2D — sampled
        Ref<ImageView> m_TransmittanceStorageView; // 2D — written

        // Two scattering volumes ping-pong the multiple-scattering iteration; one ends up the
        // running total the sky pass samples (whichever the last write landed in).
        Ref<Image> m_ScatteringImageA;
        Ref<ImageView> m_ScatteringViewA;        // Type3D — sampled
        Ref<ImageView> m_ScatteringStorageViewA; // Type3D — written
        Ref<Image> m_ScatteringImageB;
        Ref<ImageView> m_ScatteringViewB;
        Ref<ImageView> m_ScatteringStorageViewB;

        Ref<Image> m_IrradianceImage;
        Ref<ImageView> m_IrradianceView;        // 2D — sampled
        Ref<ImageView> m_IrradianceStorageView; // 2D — written

        Ref<Sampler> m_Sampler;

        // Transmittance generation (writes the 2D LUT).
        Ref<DescriptorSetLayout> m_TransmittanceSetLayout;
        Ref<PipelineLayout> m_TransmittanceLayout;
        Ref<ComputePipeline> m_TransmittancePipeline;
        Ref<DescriptorSet> m_TransmittanceSet;

        // Single-scattering generation (transmittance -> scattering A).
        Ref<DescriptorSetLayout> m_SingleSetLayout;
        Ref<PipelineLayout> m_SingleLayout;
        Ref<ComputePipeline> m_SinglePipeline;
        Ref<DescriptorSet> m_SingleSet;

        // Multiple-scattering iteration (prev total -> next total), one set per ping-pong direction.
        Ref<DescriptorSetLayout> m_MultiSetLayout;
        Ref<PipelineLayout> m_MultiLayout;
        Ref<ComputePipeline> m_MultiPipeline;
        Ref<DescriptorSet> m_MultiSetAtoB; // reads A, writes B
        Ref<DescriptorSet> m_MultiSetBtoA; // reads B, writes A

        // Irradiance generation (scattering total -> 2D LUT).
        Ref<DescriptorSetLayout> m_IrradianceSetLayout;
        Ref<PipelineLayout> m_IrradianceLayout;
        Ref<ComputePipeline> m_IrradiancePipeline;
        Ref<DescriptorSet> m_IrradianceSet;

        // The set the sky pass binds (set 1): scattering total, transmittance, linear sampler.
        Ref<DescriptorSetLayout> m_ConsumerSetLayout;
        Ref<DescriptorSet> m_ConsumerSet;

        // Which volume holds the running total after the last Generate (A after an even number of
        // multiple-scattering passes, since single scattering seeds A). The consumer set rebinds
        // to it each Generate.
        bool m_TotalInA = true;

        bool m_Initialized = false;
    };
}
