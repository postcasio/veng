#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
    class Buffer;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    struct SceneRendererSettings;
    struct ShadowConstantsBlock;
    struct PunctualShadowBlock;

    /// @brief Owns the renderer's set-1 shadow descriptor system and the punctual shadow atlas.
    ///
    /// The shadow vertical the deferred lighting pass consumes at set 1: the shared immutable
    /// comparison sampler (hardware SampleCmp), the set-1 descriptor layout + set (cascade atlas,
    /// comparison sampler, the ShadowConstants and PunctualShadowBlock dynamic uniforms, the
    /// punctual atlas), the punctual shadow atlas image/view, the 1×1 dummy atlas bound when no
    /// shadow pass is wired, the debug shadow-blit layout/set/sampler, and both constants rings.
    /// The directional cascade atlas is not owned here — ShadowScenePass owns it and Rebuild binds
    /// its view (or the dummy) into the set. All of set 1 is held off the set-0 bindless registry
    /// (a comparison sampler mistranslates in a Metal argument buffer on MoltenVK, and a closed
    /// producer→consumer resource needs no global registration), so this system holds no bindless
    /// slots and its destructor releases none.
    class ShadowSystem
    {
    public:
        /// @brief Creates the set-1 shadow system and sizes the punctual atlas from the settings.
        /// @param context  The render context the resources are created on.
        /// @param settings The renderer settings; PunctualShadowResolution sizes the punctual atlas.
        /// @return A new ShadowSystem.
        static Unique<ShadowSystem> Create(Context& context, const SceneRendererSettings& settings);

        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~ShadowSystem();

        ShadowSystem(const ShadowSystem&) = delete;
        ShadowSystem& operator=(const ShadowSystem&) = delete;

        /// @brief The largest directional ShadowResolution the device supports.
        ///
        /// The directional atlas is widest at the largest cascade grid, so a tile larger than the
        /// device image limit divided by that grid's larger dimension would overflow it.
        /// @param context The render context whose image limit bounds the atlas.
        /// @return The maximum valid ShadowResolution, in texels.
        [[nodiscard]] static u32 GetMaxShadowResolution(Context& context);

        /// @brief The largest PunctualShadowResolution the device supports.
        ///
        /// The punctual atlas tiles CubeFaceCount columns × MaxShadowedPunctual rows, so its widest
        /// side is CubeFaceCount · resolution.
        /// @param context The render context whose image limit bounds the atlas.
        /// @return The maximum valid PunctualShadowResolution, in texels.
        [[nodiscard]] static u32 GetMaxPunctualShadowResolution(Context& context);

        /// @brief Clamps a settings block's shadow resolutions to the device-supported maxima.
        ///
        /// Called before any atlas is sized so an over-large request degrades to the largest valid
        /// atlas rather than a fatal driver error.
        /// @param context  The render context whose image limit bounds the atlases.
        /// @param settings The settings whose ShadowResolution/PunctualShadowResolution are clamped.
        static void ClampResolutions(Context& context, SceneRendererSettings& settings);

        /// @brief Recreates the punctual shadow atlas image and view at the settings' resolution.
        ///
        /// Sized at PunctualShadowResolution × (MaxShadowedPunctual·CubeFaceCount) tiles; the
        /// following RebuildSets binds the new view into the recreated shadow set (binding 4).
        /// @param settings The renderer settings; PunctualShadowResolution sizes the atlas.
        void Reconfigure(const SceneRendererSettings& settings);

        /// @brief Recreates the set-1 shadow set and the debug-blit set, writing every binding.
        ///
        /// Supplies either the wired shadow pass's atlas or the dummy when shadows are off. A
        /// rebuild can land while a prior frame's command buffer still references the old sets, and
        /// the bindings carry no update-after-bind flags, so the sets are replaced (the old ones
        /// retiring through the per-frame bin) rather than updated in place.
        /// @param atlasView The directional cascade atlas view, or the dummy when shadows are off.
        void RebuildSets(const Ref<ImageView>& atlasView);

        /// @brief Writes this frame's ShadowConstants and PunctualShadowBlock into their ring regions.
        ///
        /// Touches only the current (not-yet-submitted) frame-in-flight region of each ring; the
        /// dynamic offset at bind (frame · stride) selects it.
        /// @param frameIndex The current frame-in-flight, indexing both rings.
        /// @param constants  The directional cascade constants for this frame.
        /// @param punctual   The per-light punctual shadow records for this frame.
        void WriteFrameConstants(u32 frameIndex, const ShadowConstantsBlock& constants,
                                 const PunctualShadowBlock& punctual);

        /// @brief The set-1 shadow descriptor-set layout the lighting pipelines reserve.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const { return m_SetLayout; }

        /// @brief The set-1 shadow descriptor set the lighting pass binds (recreated by every RebuildSets).
        [[nodiscard]] const Ref<DescriptorSet>& GetSet() const { return m_Set; }

        /// @brief The debug shadow-blit descriptor-set layout the blit pipeline reserves.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetBlitSetLayout() const
        {
            return m_BlitSetLayout;
        }

        /// @brief The debug shadow-blit descriptor set (recreated by every RebuildSets).
        [[nodiscard]] const Ref<DescriptorSet>& GetBlitSet() const { return m_BlitSet; }

        /// @brief The 1×1 dummy atlas view bound into the set when no shadow pass is wired.
        [[nodiscard]] const Ref<ImageView>& GetDummyView() const { return m_DummyView; }

        /// @brief The punctual shadow atlas view (set 1 binding 4).
        [[nodiscard]] const Ref<ImageView>& GetPunctualView() const { return m_PunctualView; }

        /// @brief Stride in bytes between ShadowConstants ring regions.
        [[nodiscard]] u32 GetConstantsRingStride() const { return m_ConstantsRingStride; }

        /// @brief Stride in bytes between PunctualShadowBlock ring regions.
        [[nodiscard]] u32 GetPunctualRingStride() const { return m_PunctualRingStride; }

    private:
        ShadowSystem(Context& context, const SceneRendererSettings& settings);

        /// @brief Creates the punctual atlas image + view and clears it to full visibility.
        void CreatePunctualAtlas(const SceneRendererSettings& settings);

        Context& m_Context;

        /// @brief Frames-in-flight the rings are sized for (derived from the context).
        u32 m_FramesInFlight = 0;

        /// @brief Immutable hardware comparison sampler for SampleCmp (baked into the set layout).
        Ref<Sampler> m_ComparisonSampler;

        /// @brief Layout of the set-1 shadow descriptor set. Long-lived past any Reconfigure.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief Set-1 shadow descriptor set (both lighting pipelines); replaced by every RebuildSets.
        Ref<DescriptorSet> m_Set;

        /// @brief Layout of the debug shadow-atlas blit set. Long-lived past any Reconfigure.
        Ref<DescriptorSetLayout> m_BlitSetLayout;
        /// @brief Debug shadow-atlas blit's descriptor set; replaced by every RebuildSets.
        Ref<DescriptorSet> m_BlitSet;
        /// @brief Ordinary clamp sampler for the debug blit's raw depth reads.
        Ref<Sampler> m_BlitSampler;

        /// @brief 1×1 D32 dummy atlas cleared to depth = 0 (reverse-Z far = full visibility) for the off path.
        Ref<Image> m_DummyImage;
        /// @brief View over m_DummyImage.
        Ref<ImageView> m_DummyView;

        /// @brief ShadowConstants ring buffer (set 1 binding 2), host-visible + persistently mapped.
        Ref<Buffer> m_ConstantsBuffer;
        /// @brief Stride in bytes between ShadowConstants ring regions.
        u32 m_ConstantsRingStride = 0;

        /// @brief Punctual shadow atlas (set 1 binding 4), a D32 2D atlas of the tile grid.
        Ref<Image> m_PunctualImage;
        /// @brief View over m_PunctualImage.
        Ref<ImageView> m_PunctualView;

        /// @brief PunctualShadowBlock ring buffer (set 1 binding 3), host-visible + persistently mapped.
        Ref<Buffer> m_PunctualBuffer;
        /// @brief Stride in bytes between PunctualShadowBlock ring regions.
        u32 m_PunctualRingStride = 0;
    };
}
