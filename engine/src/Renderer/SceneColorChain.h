#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <algorithm>
#include <span>

#include "Passes/SceneColorCopyScenePass.h"

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class GraphicsPipeline;
    class ComputePipeline;
    class DescriptorSet;
    class DescriptorSetLayout;
    class PipelineLayout;
    class Sampler;
    class ScenePass;

    /// @brief A copy of the scene colour and the halving chain that blurs it, with the passes that
    ///        fill both.
    ///
    /// The chain is what lets a consumer read the scene behind it at a level rather than only sharp:
    /// level 0 is the copy, each further level halves the one above with a four-tap tent, and the
    /// coarse tail is one compute dispatch (MipTailFirstLevel). It carries its own sampler, linear
    /// between levels and with no LOD clamp, since the renderer's shared sampler pins a sample to the
    /// top two levels. The refraction grab owns one ahead of the translucent pass; the pre-bloom GUI
    /// overlay pass owns another, copied ahead of the overlays it frosts.
    ///
    /// Creating the chain builds its pipelines; Recreate allocates the image at an extent and level
    /// count, and Release drops it while keeping the pipelines.
    class SceneColorChain
    {
    public:
        /// @brief Creates the chain's copy, halving and coarse-tail pipelines (Recreate allocates the image).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the chain's shaders.
        /// @param name    Debug name for the chain's image and views.
        /// @return A new SceneColorChain with no image allocated.
        static Unique<SceneColorChain> Create(Context& context, AssetManager& assets,
                                              string_view name);

        /// @brief Releases the chain's bindless slots; the images retire through the frame bin.
        ~SceneColorChain();

        SceneColorChain(const SceneColorChain&) = delete;
        SceneColorChain& operator=(const SceneColorChain&) = delete;

        /// @brief The number of levels a blurring chain carries at @p extent.
        ///
        /// The chain stops short of a single texel: its coarsest level holds roughly an 8-pixel edge
        /// on the long axis, since a level already flat across a pane buys nothing and each further
        /// level is a pass and a barrier. At least 1.
        /// @param extent The chain's level-0 extent.
        /// @return The level count.
        [[nodiscard]] static u32 BlurLevelsFor(uvec2 extent);

        /// @brief Allocates the chain at @p extent with @p levels levels, releasing any previous one.
        /// @param extent The level-0 extent.
        /// @param levels The level count; 1 is a plain copy with no blur.
        void Recreate(uvec2 extent, u32 levels);

        /// @brief Releases the chain's image and slots, keeping its pipelines.
        void Release();

        /// @brief Contributes the copy, every halving pass and the coarse-tail dispatch into @p passes.
        /// @param passes       The pass list to append into.
        /// @param namePrefix   The passes' name prefix ("<prefix> Copy", "<prefix> Downsample N").
        /// @param sourceId     The scene-colour id the copy reads.
        /// @param sourceHandle Bindless slot for the source.
        /// @param sourceSampler Sampler the copy reads the source through.
        /// @param mipIds       One id per level; index 0 is the copy's target.
        /// @param region       Which part of the allocation holds content each frame.
        /// @param sampled      A per-frame flag, set when something reads the chain this frame;
        ///                     every pass declared here skips the frames it is clear.
        void Declare(vector<Unique<ScenePass>>& passes, string_view namePrefix, ResourceId sourceId,
                     TextureHandle sourceHandle, SamplerHandle sourceSampler,
                     std::span<const ResourceId> mipIds, SceneColorChainRegion region,
                     const bool* sampled) const;

        /// @brief Whether an image is allocated.
        [[nodiscard]] bool IsAllocated() const { return m_Image != nullptr; }

        /// @brief The allocated level-0 extent; zero when unallocated.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief The whole-chain view, which a consumer samples a level of.
        [[nodiscard]] const Ref<ImageView>& GetView() const { return m_View; }

        /// @brief Bindless slot for the whole-chain view.
        [[nodiscard]] TextureHandle GetHandle() const { return m_Handle; }

        /// @brief The per-level views, base first: what the graph attaches and each halving reads.
        [[nodiscard]] const vector<Ref<ImageView>>& GetMipViews() const { return m_MipViews; }

        /// @brief How many levels the chain carries; 1 when it is a plain copy or unallocated.
        [[nodiscard]] u32 GetMipCount() const
        {
            return static_cast<u32>(std::max<usize>(m_MipViews.size(), 1));
        }

        /// @brief The chain's sampler slot: clamped edges, linear between levels, no LOD clamp.
        [[nodiscard]] SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

    private:
        SceneColorChain(Context& context, AssetManager& assets, string_view name);

        /// @brief The render context the chain's resources live on.
        Context& m_Context;
        /// @brief Debug name for the image and its views.
        string m_Name;
        /// @brief The copy pipeline (sub-rect-mapped passthrough) writing level 0.
        Ref<GraphicsPipeline> m_CopyPipeline;
        /// @brief Layout for m_CopyPipeline: a texture + sampler + sub-rect push block.
        Ref<PipelineLayout> m_CopyLayout;
        /// @brief The halving pipeline that writes one level from the one above it.
        Ref<GraphicsPipeline> m_DownsamplePipeline;
        /// @brief Layout for m_DownsamplePipeline: a texture + sampler + sub-rect push block.
        Ref<PipelineLayout> m_DownsampleLayout;
        /// @brief The coarse-tail pipeline: the chain's smallest levels halved in one workgroup.
        Ref<ComputePipeline> m_TailPipeline;
        /// @brief Layout for m_TailPipeline: the tail set + its push block.
        Ref<PipelineLayout> m_TailLayout;
        /// @brief Tail set layout: the level before the tail sampled (0), the chain sampler (1), a
        ///        storage view per tail slot (2 on).
        Ref<DescriptorSetLayout> m_TailSetLayout;
        /// @brief The chain sampler's shared slot. Shared, so it is never released.
        SamplerHandle m_SamplerHandle;
        /// @brief The chain sampler itself, which the coarse-tail set binds.
        Ref<Sampler> m_Sampler;

        /// @brief The allocated level-0 extent.
        uvec2 m_Extent{0, 0};
        /// @brief The chain image.
        Ref<Image> m_Image;
        /// @brief Whole-chain view over m_Image.
        Ref<ImageView> m_View;
        /// @brief Bindless slot for m_View.
        TextureHandle m_Handle;
        /// @brief One single-level view per level: the graph's attachment and its read source.
        vector<Ref<ImageView>> m_MipViews;
        /// @brief Bindless slot per level, so a halving pass can sample the level above it.
        vector<TextureHandle> m_MipHandles;
        /// @brief The tail's set for the current chain, or null when the chain has no tail.
        Ref<DescriptorSet> m_TailSet;
        /// @brief The tail's first level (MipTailFirstLevel), meaningful while m_TailSet is set.
        u32 m_TailFirst = 0;
    };
}
