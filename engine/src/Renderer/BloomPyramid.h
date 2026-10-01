#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <vector>

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
    enum class BloomKernel : u8;

    /// @brief Owns the compute mip-pyramid bloom battery — resources, pipelines, and sweep.
    ///
    /// The post-lighting bloom vertical the renderer wires ahead of tonemap: the HDR mip-chain
    /// pyramid image with its per-level views, the clamp linear sampler, the four compute pipelines (Cod/Kawase down/up) with their set layout and
    /// per-level descriptor sets, and the mip-0 bindless slot. Declare contributes the down/up
    /// sweep; the tonemap adds the accumulated mip 0 into the scene colour itself, sampling it
    /// through GetMip0Handle and GetMip0SampleMap. The down-pass threshold divides by the frame's
    /// resolved exposure, the one cross-battery read (from AutoExposureMeter). The filter kernel
    /// is a construction-time choice re-applied on Reconfigure. All of set 1 is held off the set-0
    /// bindless registry (the closed bloom chain needs no global registration, and a dedicated set
    /// sidesteps the set-0 storage-image argument-buffer path on MoltenVK).
    ///
    /// The pyramid begins at half the scene extent: the bright pass is the chain's first 2:1
    /// downsample, so no level is a full-resolution surface, and the tonemap's bilinear tap
    /// magnifies mip 0 on the way into the add.
    class BloomPyramid
    {
    public:
        /// @brief Creates the bloom pipelines, set layouts, and sampler (the pyramid is built by Resize).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the bloom compute shaders.
        /// @param kernel  The initial down/up filter kernel (Cod or Kawase).
        /// @return A new BloomPyramid.
        static Unique<BloomPyramid> Create(Context& context, AssetManager& assets,
                                           BloomKernel kernel);

        /// @brief Releases the mip-0 bindless slot; the images retire through the frame bin.
        ~BloomPyramid();

        BloomPyramid(const BloomPyramid&) = delete;
        BloomPyramid& operator=(const BloomPyramid&) = delete;

        /// @brief Builds the pyramid, views, and per-level sets for a scene extent.
        ///
        /// Builds the HDR mip chain at half @p sceneExtent (BloomPyramidBase, stopping a few
        /// levels short of 1×1), the per-mip views, and the down/up descriptor sets; the level-0
        /// down set binds @p hdrView, so this runs after the HDR target is recreated. Mip 0
        /// registers into bindless for the tonemap's bloom read. A call at the extent the pyramid
        /// already holds keeps the chain and only re-points the level-0 source.
        /// @param sceneExtent The post-resolve allocation the bright pass reads.
        /// @param hdrView     The live scene-colour target the level-0 down set binds.
        void Resize(uvec2 sceneExtent, const Ref<ImageView>& hdrView);

        /// @brief Drops the pyramid, its views, sets, and mip-0 slot, keeping the pipelines.
        ///
        /// A renderer with bloom inactive holds no chain; a later Resize rebuilds it.
        void Release();

        /// @brief Re-applies the down/up filter kernel choice (Cod or Kawase).
        /// @param kernel The kernel selected this frame; read by Declare at record time.
        void Reconfigure(BloomKernel kernel) { m_Kernel = kernel; }

        /// @brief Re-points the level-0 down set at a new scene-color source.
        ///
        /// Resize binds the raw HDR target, but a pre-bloom post-process effect chain replaces the
        /// scene color Declare reads (its `hdrId`) with the effect chain's final target. The bright
        /// pass samples this view; the id passed to Declare must resolve to the same image, or the
        /// derived barrier and the sampled image diverge. Cheap enough to call each Rebuild (one
        /// descriptor set, no realloc).
        /// @param source The live scene-color view Declare's `hdrId` resolves to this Rebuild.
        void SetSourceView(const Ref<ImageView>& source);

        /// @brief Declares the down/up compute sweep into the graph ahead of tonemap.
        ///
        /// Down-sweep (level 0..N-1, barrier between levels), then the in-place tent up-sweep
        /// (level N-2..0, barrier between levels), leaving the accumulated bloom in mip 0. Per-frame
        /// Threshold / Radius ride the compute push, read from the SceneView at record time; the
        /// down-pass threshold divides by @p autoExposure's resolved exposure.
        ///
        /// Level 0 additionally takes the larger of its luminance bright-pass and the bloom mask,
        /// the screen-space target a forward material writes to name the glow it wants apart from
        /// how bright it is. The mask reaches the tail at the scene colour's own allocation —
        /// promoted alongside it when the scene rasterized less than it — so it is read through the
        /// same sub-rect mapping as the colour beside it. An invalid mask id or slot leaves
        /// level 0 on the bright-pass alone.
        /// @param graph        The renderer's internal graph being rebuilt.
        /// @param hdrId        The HDR target import (the level-0 source).
        /// @param chainId      The per-mip pyramid import the down/up sweep reads and writes.
        /// @param autoExposure The exposure meter whose resolved exposure scales the threshold.
        /// @param maskId       The bloom-mask target import, or an invalid id when there is none.
        /// @param maskHandle   Bindless slot of the bloom-mask target.
        /// @param maskSampler  Bindless slot of the sampler the mask is read through.
        void Declare(RenderGraph& graph, ResourceId hdrId, MipChainId chainId,
                     const AutoExposureMeter& autoExposure, ResourceId maskId,
                     TextureHandle maskHandle, SamplerHandle maskSampler);

        /// @brief Bindless slot for pyramid mip 0, sampled by the tonemap and the Bloom debug blit.
        [[nodiscard]] TextureHandle GetMip0Handle() const { return m_Mip0Handle; }

        /// @brief The sub-rect map a full-frame UV reads mip 0 through.
        ///
        /// The same mapping the sweep's own dispatches derive for level 0 — over the pyramid's
        /// valid base, half @p validExtent — so a reader of mip 0 lands on the texels the
        /// up-sweep wrote: `xy` the valid/allocated scale, `zw` the bilinear-tap clamp (half a
        /// texel inside the valid region).
        /// @param validExtent The frame's valid scene extent (SceneView::PostResolveExtent).
        /// @return `(scale.xy, clamp.zw)`, applied as `min(uv * map.xy, map.zw)`.
        [[nodiscard]] vec4 GetMip0SampleMap(uvec2 validExtent) const;

        /// @brief Number of mip levels in the pyramid (the import slot count and the binding range).
        [[nodiscard]] u32 GetMipCount() const { return static_cast<u32>(m_Mips.size()); }

        /// @brief The per-level storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetMipViews() const { return m_Mips; }

        /// @brief The down/up set-1 layout, shared with the SSR reflection blur (sampled + sampler + dest).
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetDownUpSetLayout() const
        {
            return m_DownUpSetLayout;
        }

    private:
        BloomPyramid(Context& context, AssetManager& assets, BloomKernel kernel);

        Context& m_Context;

        /// @brief The down/up filter kernel choice, read by Declare at record time.
        BloomKernel m_Kernel;
        /// @brief The scene extent the pyramid was built for (zero while released).
        uvec2 m_SceneExtent{0};
        /// @brief The pyramid's level-0 extent, half the scene extent (set by Resize).
        uvec2 m_Extent{1};

        /// @brief Cod bloom downsample pipeline (bright-pass + Karis on mip 0, 13-tap below).
        Ref<ComputePipeline> m_DownPipeline;
        /// @brief Cod bloom upsample-accumulate pipeline (3×3 tent into the finer level).
        Ref<ComputePipeline> m_UpPipeline;
        /// @brief Kawase bloom downsample pipeline (bright-pass + Karis on mip 0, 5-tap below).
        Ref<ComputePipeline> m_DownKawasePipeline;
        /// @brief Kawase bloom upsample-accumulate pipeline (8-tap bilinear into the finer level).
        Ref<ComputePipeline> m_UpKawasePipeline;
        /// @brief Shared layout for the down/up pipelines (the shared down/up set + push block).
        Ref<PipelineLayout> m_DownUpLayout;
        /// @brief Set-1 layout shared by down/up: sampled source (0) + sampler (1) + storage dest (2).
        Ref<DescriptorSetLayout> m_DownUpSetLayout;

        /// @brief Bloom mip-pyramid image: an HDR mip chain the compute down/up sweep operates on.
        Ref<Image> m_Image;
        /// @brief One single-mip view per pyramid level, the storage destination and sampled source.
        std::vector<Ref<ImageView>> m_Mips;
        /// @brief Clamp-to-edge linear sampler for the bilinear down/up taps.
        Ref<Sampler> m_Sampler;
        /// @brief Bindless slot for pyramid mip 0 (the tonemap and the DebugView::Bloom arm read it).
        TextureHandle m_Mip0Handle;

        /// @brief One downsample set per level k, binding level k's source and destination.
        std::vector<Ref<DescriptorSet>> m_DownSets;
        /// @brief One upsample set per finer level k, binding the coarser source (k+1) and dest (k).
        std::vector<Ref<DescriptorSet>> m_UpSets;
    };
}
