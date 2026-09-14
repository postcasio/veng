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

    /// @brief Owns the promoted scene-color target and the pipeline that writes it.
    ///
    /// The spatial half of the promotion. The scene chain finishes its HDR colour in the render
    /// allocation (or a dynamic-resolution sub-rect of it), and one fullscreen pass resamples that
    /// into this post-resolve-allocation target the HDR tail reads — so the tail runs at the
    /// post-resolve allocation in every configuration rather than only behind a temporal-upscaling
    /// resolve.
    ///
    /// Nothing is allocated while the promotion is unwired, which is every frame whose scene colour
    /// already covers the post-resolve allocation: render scale 1, or a temporal-upscaling resolve
    /// that reconstructed it itself.
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

        /// @brief The fullscreen promotion pipeline (one bilinear resample, writes HdrFormat).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

        /// @brief The promoted scene-color target the tail reads; null while unwired.
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the promoted scene target; the tail samples through it.
        [[nodiscard]] TextureHandle GetSceneHandle() const { return m_SceneHandle; }

    private:
        PostResolveUpscale(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief The fullscreen upscale pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Layout for the upscale pipeline: the texture/sampler/sub-rect push block.
        Ref<PipelineLayout> m_Layout;

        /// @brief Scene-color target the chain writes while the promotion is wired.
        Ref<Image> m_SceneImage;
        /// @brief View over m_SceneImage.
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for the scene view.
        TextureHandle m_SceneHandle;
    };
}
