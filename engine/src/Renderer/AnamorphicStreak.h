#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <array>

#include "StreakKernel.h"

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;
    class AutoExposureMeter;

    /// @brief Owns the anamorphic streak: every bright source stretched into a thin horizontal line.
    ///
    /// A prefilter takes the scene colour's bright pass — each tap conditioned and the average
    /// soft-kneed exactly as the bloom's level 0 is (`bright_tap.slang`) — into image 0, at a quarter
    /// of the frame's width and half its height (StreakExtent). A row filter then spreads each row of
    /// it along itself into image 1, one invocation per row sweeping forward and back, so every
    /// source falls off exactly as `ratio^distance` per channel (StreakFilterRow). The tonemap adds
    /// image 1 beside the bloom. The images and their sets are held off bindless but for image 1's
    /// slot, which the tonemap samples. Built with the bloom and released with it; both passes skip
    /// any frame the streak is not drawn (SetActive), so an intensity of zero costs no dispatch.
    class AnamorphicStreak
    {
    public:
        /// @brief Creates the streak's pipelines against the bloom's down/up set layout.
        /// @param context   The render context the resources are created on.
        /// @param assets    Asset manager the compute shaders load through.
        /// @param setLayout The bloom's down/up set layout (sampled source, sampler, storage dest).
        /// @return A new AnamorphicStreak.
        static Unique<AnamorphicStreak> Create(Context& context, AssetManager& assets,
                                               const Ref<DescriptorSetLayout>& setLayout);

        /// @brief Releases the final image's bindless slot; the images retire through the frame bin.
        ~AnamorphicStreak();

        AnamorphicStreak(const AnamorphicStreak&) = delete;
        AnamorphicStreak& operator=(const AnamorphicStreak&) = delete;

        /// @brief Builds the two images and their sets for a scene extent.
        ///
        /// A call at the extent already held keeps the images and only re-points the prefilter's
        /// source.
        /// @param sceneExtent The post-resolve allocation the prefilter reads.
        /// @param hdrView     The scene colour the prefilter samples.
        void Resize(uvec2 sceneExtent, const Ref<ImageView>& hdrView);

        /// @brief Drops the images, their sets and the slot, keeping the pipelines.
        void Release();

        /// @brief Re-points the prefilter at the scene colour Declare's `hdrId` resolves to.
        /// @param source The live scene-colour view.
        void SetSourceView(const Ref<ImageView>& source);

        /// @brief Declares the prefilter and the row filter into the graph.
        ///
        /// Each pass skips a frame SetActive left false; the tonemap then samples nothing from them.
        /// @param graph        The renderer's internal graph being rebuilt.
        /// @param hdrId        The scene colour the prefilter reads.
        /// @param imageIds     The two streak images' imports, in GetViews' order.
        /// @param autoExposure The exposure meter whose resolved exposure scales the threshold and
        ///                     the ceiling, as bloom's.
        void Declare(RenderGraph& graph, ResourceId hdrId,
                     const std::array<ResourceId, 2>& imageIds,
                     const AutoExposureMeter& autoExposure);

        /// @brief Sets whether this frame draws the streak; read by every pass's skip predicate.
        /// @param active True when the tonemap will add the streak this frame.
        void SetActive(bool active) { m_Active = active; }

        /// @brief The index into GetViews of the image the row filter writes, which the tonemap reads.
        [[nodiscard]] static constexpr u32 GetFinalImage() { return 1; }

        /// @brief The two images' views, bound to their imports each Execute.
        [[nodiscard]] const std::array<Ref<ImageView>, 2>& GetViews() const { return m_Views; }

        /// @brief Bindless slot of the final image, which the tonemap samples.
        [[nodiscard]] TextureHandle GetFinalHandle() const { return m_FinalHandle; }

        /// @brief The scene extent the images were built for (zero while released).
        [[nodiscard]] uvec2 GetSceneExtent() const { return m_SceneExtent; }

    private:
        AnamorphicStreak(Context& context, AssetManager& assets,
                         const Ref<DescriptorSetLayout>& setLayout);

        Context& m_Context;

        /// @brief The bloom's down/up set layout every streak set is built against.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The prefilter's pipeline layout (the set layout + its push block).
        Ref<PipelineLayout> m_PrefilterLayout;
        /// @brief The row filter's pipeline layout (the set layout + its push block).
        Ref<PipelineLayout> m_FilterLayout;
        /// @brief The bright-pass prefilter into image 0.
        Ref<ComputePipeline> m_PrefilterPipeline;
        /// @brief The row filter.
        Ref<ComputePipeline> m_FilterPipeline;

        /// @brief The scene extent the images were built for (zero while released).
        uvec2 m_SceneExtent{0};
        /// @brief Image 0, the prefilter's output, and image 1, the row filter's.
        std::array<Ref<Image>, 2> m_Images;
        /// @brief One full view per image, the storage destination and sampled source both.
        std::array<Ref<ImageView>, 2> m_Views;
        /// @brief Clamp-to-edge linear sampler the prefilter's bilinear taps read through.
        Ref<Sampler> m_Sampler;
        /// @brief The prefilter's set: the scene colour, the sampler, image 0.
        Ref<DescriptorSet> m_PrefilterSet;
        /// @brief The row filter's set: image 0, the sampler, image 1.
        Ref<DescriptorSet> m_FilterSet;
        /// @brief Bindless slot of the final image.
        TextureHandle m_FinalHandle;
        /// @brief Whether this frame draws the streak; false skips every pass.
        bool m_Active = false;
    };
}
