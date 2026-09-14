#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Veng.h>

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

    /// @brief Owns the promoted scene-color and bloom-mask targets and the pipelines that write them.
    ///
    /// The spatial half of the promotion. The scene chain finishes its HDR colour in the render
    /// allocation (or a dynamic-resolution sub-rect of it), and one fullscreen pass resamples that
    /// into this post-resolve-allocation target the HDR tail reads — so the tail runs at the
    /// post-resolve allocation in every configuration rather than only behind a temporal-upscaling
    /// resolve.
    ///
    /// The bloom mask is the scene colour's companion channel and crosses the same boundary through
    /// a second target and a second pipeline here: the translucent pass rasterizes it into the
    /// render allocation, and a pre-bloom overlay composite adds to it at the post-resolve one. The
    /// two sides latch independently, because a temporal-upscaling resolve promotes the colour by
    /// reconstructing it and leaves the mask where the scene wrote it.
    ///
    /// Neither target is allocated while its side of the promotion is unwired, which for the colour
    /// is every frame whose scene colour already covers the post-resolve allocation (render scale 1,
    /// or a temporal-upscaling resolve that reconstructed it itself) and for the mask is every frame
    /// whose rendered sub-rect already covers it.
    class PostResolveUpscale
    {
    public:
        /// @brief Creates the upscale pipeline (the target is built by Resize).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the upscale fragment shader.
        /// @return A new PostResolveUpscale.
        static Unique<PostResolveUpscale> Create(Context& context, AssetManager& assets);

        /// @brief Releases the scene target's bindless slot; the image retires through the frame bin.
        ~PostResolveUpscale();

        PostResolveUpscale(const PostResolveUpscale&) = delete;
        PostResolveUpscale& operator=(const PostResolveUpscale&) = delete;

        /// @brief Recreates the promoted scene target at @p extent, or releases it when unwired.
        ///
        /// The target is the post-resolve allocation (HdrFormat) like every other scene-color
        /// intermediate on that side; the promotion pass writes all of it.
        /// @param extent  The post-resolve allocation the target is sized to.
        /// @param enabled Whether the promotion is wired (the target is allocated only then).
        void Resize(uvec2 extent, bool enabled);

        /// @brief Recreates the promoted bloom-mask target at @p extent, or releases it when unwired.
        ///
        /// The single-channel companion to Resize, on its own latch: the mask promotion runs
        /// whenever the rendered sub-rect is not already the post-resolve allocation, which includes
        /// the temporal-upscaling frames the colour promotion sits out.
        /// @param extent  The post-resolve allocation the target is sized to.
        /// @param enabled Whether the mask promotion is wired (the target is allocated only then).
        void ResizeMask(uvec2 extent, bool enabled);

        /// @brief The fullscreen promotion pipeline (one bilinear resample, writes HdrFormat).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

        /// @brief The fullscreen mask-promotion pipeline (the same resample, writes BloomMaskFormat).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetMaskPipeline() const
        {
            return m_MaskPipeline;
        }

        /// @brief The promoted scene-color target the tail reads; null while unwired.
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the promoted scene target; the tail samples through it.
        [[nodiscard]] TextureHandle GetSceneHandle() const { return m_SceneHandle; }

        /// @brief The promoted bloom-mask target the tail writes and bloom reads; null while unwired.
        [[nodiscard]] const Ref<ImageView>& GetMaskView() const { return m_MaskView; }

        /// @brief Bindless slot for the promoted bloom-mask target; the bright pass samples it.
        [[nodiscard]] TextureHandle GetMaskHandle() const { return m_MaskHandle; }

    private:
        PostResolveUpscale(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief The fullscreen upscale pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The fullscreen upscale pipeline writing the single-channel mask format.
        Ref<GraphicsPipeline> m_MaskPipeline;
        /// @brief Layout for both upscale pipelines: the texture/sampler/sub-rect push block.
        Ref<PipelineLayout> m_Layout;

        /// @brief Scene-color target the chain writes while the promotion is wired.
        Ref<Image> m_SceneImage;
        /// @brief View over m_SceneImage.
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for the scene view.
        TextureHandle m_SceneHandle;

        /// @brief Bloom-mask target the tail writes while the mask promotion is wired.
        Ref<Image> m_MaskImage;
        /// @brief View over m_MaskImage.
        Ref<ImageView> m_MaskView;
        /// @brief Bindless slot for the mask view.
        TextureHandle m_MaskHandle;
    };
}
