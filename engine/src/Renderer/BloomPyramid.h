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
    /// pyramid image with its per-level storage views and whole-chain sample view, the clamp
    /// linear sampler, the composite result image, the five compute pipelines (Cod/Kawase
    /// down/up + composite) with their two set layouts and per-level descriptor sets, and the
    /// result + mip-0 bindless slots. Declare contributes the down/up/composite sweep; the
    /// down-pass threshold divides by the frame's resolved exposure, the one cross-battery read
    /// (from AutoExposureMeter). The filter kernel is a construction-time choice re-applied on
    /// Reconfigure. All of set 1 is held off the set-0 bindless registry (the closed bloom chain
    /// needs no global registration, and a dedicated set sidesteps the set-0 storage-image
    /// argument-buffer path on MoltenVK).
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

        /// @brief Releases the result/mip-0 bindless slots; the images retire through the frame bin.
        ~BloomPyramid();

        BloomPyramid(const BloomPyramid&) = delete;
        BloomPyramid& operator=(const BloomPyramid&) = delete;

        /// @brief Recreates the extent-sized pyramid, result image, views, and per-level sets.
        ///
        /// Builds the HDR mip chain (stopping a few levels short of 1×1), the per-mip storage
        /// views, the whole-chain sampled view, the composite result, and the down/up + composite
        /// descriptor sets; the level-0 down set and the composite set bind @p hdrView, so this
        /// runs after the HDR target is recreated. The result view registers into bindless for the
        /// tonemap sample.
        /// @param extent  The allocation extent the pyramid is sized to.
        /// @param hdrView The live HDR target the level-0 source and composite sets bind.
        void Resize(uvec2 extent, const Ref<ImageView>& hdrView);

        /// @brief Re-applies the down/up filter kernel choice (Cod or Kawase).
        /// @param kernel The kernel selected this frame; read by Declare at record time.
        void Reconfigure(BloomKernel kernel) { m_Kernel = kernel; }

        /// @brief Declares the down/up/composite compute sweep into the graph ahead of tonemap.
        ///
        /// Down-sweep (level 0..N-1, barrier between levels), in-place tent up-sweep (level
        /// N-2..0, barrier between levels), then the composite into the result. Per-frame
        /// Threshold / Intensity / Radius ride the compute push, read from the SceneView at record
        /// time; the down-pass threshold divides by @p autoExposure's resolved exposure.
        /// @param graph        The renderer's internal graph being rebuilt.
        /// @param hdrId        The HDR target import (level-0 source and composite input).
        /// @param chainId      The per-mip pyramid import the down/up sweep reads and writes.
        /// @param resultId     The composite result import.
        /// @param autoExposure The exposure meter whose resolved exposure scales the threshold.
        void Declare(RenderGraph& graph, ResourceId hdrId, MipChainId chainId, ResourceId resultId,
                     const AutoExposureMeter& autoExposure);

        /// @brief The composite result the tonemap stage reads when bloom is on.
        [[nodiscard]] const Ref<ImageView>& GetResultView() const { return m_ResultView; }

        /// @brief Bindless slot for the composite result view (the tonemap samples it).
        [[nodiscard]] TextureHandle GetResultHandle() const { return m_ResultHandle; }

        /// @brief Bindless slot for pyramid mip 0 (the DebugView::Bloom arm blits it).
        [[nodiscard]] TextureHandle GetMip0Handle() const { return m_Mip0Handle; }

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
        /// @brief The allocation extent the pyramid is sized to (set by Resize).
        uvec2 m_Extent{1};

        /// @brief Cod bloom downsample pipeline (bright-pass + Karis on mip 0, 13-tap below).
        Ref<ComputePipeline> m_DownPipeline;
        /// @brief Cod bloom upsample-accumulate pipeline (3×3 tent into the finer level).
        Ref<ComputePipeline> m_UpPipeline;
        /// @brief Kawase bloom downsample pipeline (bright-pass + Karis on mip 0, 5-tap below).
        Ref<ComputePipeline> m_DownKawasePipeline;
        /// @brief Kawase bloom upsample-accumulate pipeline (8-tap bilinear into the finer level).
        Ref<ComputePipeline> m_UpKawasePipeline;
        /// @brief Bloom composite compute pipeline (hdr + mip0 * Intensity → result).
        Ref<ComputePipeline> m_CompositePipeline;
        /// @brief Shared layout for the down/up pipelines (the shared down/up set + push block).
        Ref<PipelineLayout> m_DownUpLayout;
        /// @brief Layout for the composite pipeline (the distinct composite set + push block).
        Ref<PipelineLayout> m_CompositeLayout;
        /// @brief Set-1 layout shared by down/up: sampled source (0) + sampler (1) + storage dest (2).
        Ref<DescriptorSetLayout> m_DownUpSetLayout;
        /// @brief Set-1 layout for composite: two sampled inputs (0,1) + sampler (2) + storage dest (3).
        Ref<DescriptorSetLayout> m_CompositeSetLayout;

        /// @brief Bloom mip-pyramid image: an HDR mip chain the compute down/up sweep operates on.
        Ref<Image> m_Image;
        /// @brief One single-mip storage view per pyramid level (the down/up dispatches write each).
        std::vector<Ref<ImageView>> m_Mips;
        /// @brief Whole-chain sampled view of the pyramid; the down/up dispatches read a level by LOD.
        Ref<ImageView> m_SampleView;
        /// @brief Clamp-to-edge linear sampler for the bilinear down/up taps.
        Ref<Sampler> m_Sampler;
        /// @brief Bloom composite result image (full extent); the tonemap samples it when bloom is on.
        Ref<Image> m_ResultImage;
        /// @brief View over m_ResultImage.
        Ref<ImageView> m_ResultView;
        /// @brief Bindless slot for the composite result view.
        TextureHandle m_ResultHandle;
        /// @brief Bindless slot for pyramid mip 0 (the DebugView::Bloom arm blits it).
        TextureHandle m_Mip0Handle;

        /// @brief One downsample set per level k, binding level k's source and destination.
        std::vector<Ref<DescriptorSet>> m_DownSets;
        /// @brief One upsample set per finer level k, binding the coarser source (k+1) and dest (k).
        std::vector<Ref<DescriptorSet>> m_UpSets;
        /// @brief Composite set: HDR + bloom mip 0 sampled inputs and the result storage dest.
        Ref<DescriptorSet> m_CompositeSet;
    };
}
