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

    /// @brief Owns the sub-rect scene-color target and the pipeline that promotes it to the allocation.
    ///
    /// The non-temporal half of the resolve anchor. With no temporal resolve wired, the scene
    /// chain rasterizes into this target's dynamic-resolution sub-rect and one fullscreen pass
    /// upscales it into the allocation-sized scene color the HDR tail reads — so the tail runs at
    /// the allocation in every configuration rather than only behind a temporal resolve.
    ///
    /// Nothing is allocated while the promotion is unwired, which is every frame a viewport renders
    /// at its allocation scale: a render scale expressed statically is already the allocation, so
    /// only a dynamic-resolution frame actually below its ceiling holds this memory.
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

        /// @brief Recreates the sub-rect scene target at @p extent, or releases it when unwired.
        ///
        /// The target is allocation-sized (HdrFormat) like every other scene-color intermediate;
        /// the frame's render scale selects the sub-rect of it the scene rasterizes into.
        /// @param extent  The allocation extent the target is sized to.
        /// @param enabled Whether the promotion is wired (the target is allocated only then).
        void Resize(uvec2 extent, bool enabled);

        /// @brief The fullscreen upscale pipeline (sub-rect bilinear resample, writes HdrFormat).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

        /// @brief The sub-rect scene-color target the scene chain writes; null while unwired.
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the sub-rect scene target; the upscale samples through it.
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
