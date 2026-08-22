#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <algorithm>
#include <span>

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
    class PipelineLayout;
    class ScenePass;
    struct SceneRendererSettings;

    /// @brief Owns the pre-translucent refraction grab — the scene-color/depth copy and its pipeline.
    ///
    /// The scene-color copy vertical the renderer wires ahead of the translucent pass: the
    /// full-extent HdrFormat scene-color intermediate and the R32Sfloat opaque-depth intermediate a
    /// refractive material samples through the view block's SceneColor handles, plus the two-attachment
    /// copy pipeline that fills them. Recreate allocates the pair (and registers their bindless slots)
    /// only when Settings.Refraction is set, releasing them otherwise; Declare contributes the copy
    /// pass into the renderer's pass list at the position the renderer grabs the lit scene color. The
    /// copy pipeline is built unconditionally in the constructor — it is refraction-only and stays
    /// created regardless of the toggle.
    class RefractionGrab
    {
    public:
        /// @brief Creates the scene-color copy pipeline and layout (the intermediates are built by Recreate).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the scene-color copy fragment shader.
        /// @return A new RefractionGrab.
        static Unique<RefractionGrab> Create(Context& context, AssetManager& assets);

        /// @brief Releases the scene-color/depth bindless slots; the images retire through the frame bin.
        ~RefractionGrab();

        RefractionGrab(const RefractionGrab&) = delete;
        RefractionGrab& operator=(const RefractionGrab&) = delete;

        /// @brief Recreates the scene-color and depth intermediates at the current extent.
        ///
        /// Allocates the full-extent HdrFormat scene-color target and R32Sfloat depth target and
        /// registers their bindless slots when Settings.Refraction is set; otherwise releases any
        /// previously-created pair. Called from the renderer's Create and every Resize/Configure.
        /// @param settings The active renderer settings (Refraction gates allocation).
        /// @param extent   The allocation extent the intermediates are sized to.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent);

        /// @brief Contributes the pre-translucent scene-color copy pass into the pass list.
        ///
        /// Appends a SceneColorCopyScenePass (over the owned copy pipeline) that grabs the lit scene
        /// color and opaque depth into the intermediates the translucent pass then samples. The
        /// renderer calls this at both Rebuild arms that composite the scene, ahead of the translucent
        /// pass, when refraction is active.
        /// @param passes       The renderer's pass list to append into.
        /// @param sourceId     Lit scene-color source id (whichever intermediate the TAA/SSR routing picked).
        /// @param sourceHandle Bindless slot for the lit source.
        /// @param depthId      Opaque depth source id.
        /// @param depthHandle  Bindless slot for the opaque depth.
        /// @param copyId       The scene-color grab target this pass writes (the renderer's import).
        /// @param depthCopyId  The depth-copy target this pass writes (the renderer's import).
        /// @param sampler      Shared sampler bindless slot.
        /// @param extent       The current render extent.
        /// @param mipIds       One id per chain level; index 0 is `copyId`, the rest are the
        ///                     halving levels. Empty or single-entry when the blur is off.
        void Declare(vector<Unique<ScenePass>>& passes, ResourceId sourceId,
                     TextureHandle sourceHandle, ResourceId depthId, TextureHandle depthHandle,
                     ResourceId copyId, ResourceId depthCopyId, SamplerHandle sampler, uvec2 extent,
                     std::span<const ResourceId> mipIds) const;

        /// @brief The scene-color intermediate view (bound to its import when refraction is active).
        ///
        /// Spans the whole mip chain, so the view-constants handle a material samples through can
        /// select a level; the per-level views below are what the graph attaches and reads.
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief The per-level views of the scene-color chain, base first; one entry with no blur.
        [[nodiscard]] const vector<Ref<ImageView>>& GetSceneMipViews() const
        {
            return m_SceneMipViews;
        }

        /// @brief How many levels the scene-color grab carries; 1 when the blur is off.
        [[nodiscard]] u32 GetSceneMipCount() const
        {
            return static_cast<u32>(std::max<usize>(m_SceneMipViews.size(), 1));
        }

        /// @brief The sampler a material reads the chain through: clamped edges, no LOD clamp.
        ///
        /// The renderer's shared g-buffer sampler cannot serve this — it carries the default MaxLod
        /// of 1, which silently pins every blurred sample to the top two levels.
        [[nodiscard]] SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

        /// @brief The scene-depth intermediate view (bound to its import when refraction is active).
        [[nodiscard]] const Ref<ImageView>& GetDepthView() const { return m_DepthView; }

        /// @brief Bindless slot for the scene-color intermediate (written into the view-constants block).
        [[nodiscard]] TextureHandle GetSceneHandle() const { return m_SceneHandle; }

        /// @brief Bindless slot for the scene-depth intermediate (written into the view-constants block).
        [[nodiscard]] TextureHandle GetDepthHandle() const { return m_DepthHandle; }

    private:
        RefractionGrab(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief Scene-color copy pipeline (sub-rect-mapped passthrough, two attachments), writing HdrFormat + R32Sfloat.
        Ref<GraphicsPipeline> m_CopyPipeline;
        /// @brief Layout for m_CopyPipeline: a texture + sampler + sub-rect push block.
        Ref<PipelineLayout> m_CopyLayout;

        /// @brief Refraction scene-color intermediate a translucent material samples.
        Ref<Image> m_SceneImage;
        /// @brief Whole-chain view over m_SceneImage (what a material samples a level of).
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for m_SceneView.
        TextureHandle m_SceneHandle;
        /// @brief One single-level view per chain level: the graph's attachment and its read source.
        vector<Ref<ImageView>> m_SceneMipViews;
        /// @brief Bindless slot per level, so a halving pass can sample the level above it.
        vector<TextureHandle> m_SceneMipHandles;
        /// @brief The chain sampler's shared slot: clamp-to-edge, linear between levels, no LOD
        ///        clamp. Shared, so it is never released.
        SamplerHandle m_SamplerHandle;
        /// @brief The downsample pipeline that halves one level into the next.
        Ref<GraphicsPipeline> m_DownsamplePipeline;
        /// @brief Layout for m_DownsamplePipeline: a texture + sampler + sub-rect push block.
        Ref<PipelineLayout> m_DownsampleLayout;
        /// @brief Refraction scene-depth intermediate: the opaque depth copied beside the scene color.
        Ref<Image> m_DepthImage;
        /// @brief View over m_DepthImage.
        Ref<ImageView> m_DepthView;
        /// @brief Bindless slot for m_DepthView.
        TextureHandle m_DepthHandle;
    };
}
