#pragma once

#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief The fullscreen debug-blit pipelines for the non-Final DebugView arms.
    ///
    /// Each arm's terminal blit selects one of these; the layouts are held only to keep the
    /// pipelines' descriptor/push-constant declarations alive. Built once at SceneRenderer::Create.
    struct DebugBlitPipelines
    {
        /// @brief Blits the albedo g-buffer channel.
        ///
        /// Reused for the Bloom, Emissive, and Reflections sources — the source is a push value,
        /// the pipeline the same fullscreen blit.
        Ref<GraphicsPipeline> Albedo;
        /// @brief Layout the albedo blit pipeline was built against.
        Ref<PipelineLayout> AlbedoLayout;
        /// @brief Blits the world-normal g-buffer channel.
        Ref<GraphicsPipeline> Normal;
        /// @brief Layout the normal blit pipeline was built against.
        Ref<PipelineLayout> NormalLayout;
        /// @brief Blits the depth buffer as a linear grey scale.
        Ref<GraphicsPipeline> Depth;
        /// @brief Layout the depth blit pipeline was built against.
        Ref<PipelineLayout> DepthLayout;
        /// @brief Blits a packed-ORM channel (Roughness/Metallic/Occlusion), the channel a push
        /// value.
        Ref<GraphicsPipeline> Orm;
        /// @brief Layout the ORM blit pipeline was built against.
        Ref<PipelineLayout> OrmLayout;
        /// @brief Blits the SSAO target.
        Ref<GraphicsPipeline> Ao;
        /// @brief Layout the SSAO blit pipeline was built against.
        Ref<PipelineLayout> AoLayout;
        /// @brief Blits the per-object velocity target colorized as an optical-flow field.
        Ref<GraphicsPipeline> Motion;
        /// @brief Layout the motion-vector blit pipeline was built against.
        Ref<PipelineLayout> MotionLayout;
        /// @brief Blits the directional shadow atlas raw depth.
        ///
        /// Reads through a dedicated set 1, not bindless.
        Ref<GraphicsPipeline> Shadow;
        /// @brief Layout the shadow blit pipeline was built against.
        Ref<PipelineLayout> ShadowLayout;
        /// @brief Blits the depth-of-field chain's signed circle-of-confusion target as a near/far
        /// ramp.
        Ref<GraphicsPipeline> Coc;
        /// @brief Layout the circle-of-confusion blit pipeline was built against.
        Ref<PipelineLayout> CocLayout;

        /// @brief Builds every debug-blit pipeline over the shared fullscreen vertex stage.
        ///
        /// @param context              Render context the pipelines are created on.
        /// @param assets               Asset manager the blit shaders are loaded through.
        /// @param outputFormat         Color format every blit pipeline writes.
        /// @param shadowBlitSetLayout  The shadow system's dedicated blit set the shadow blit
        ///                             samples raw depth through.
        /// @return The built pipeline set.
        static Unique<DebugBlitPipelines>
        Create(Context& context, AssetManager& assets, Format outputFormat,
               const Ref<DescriptorSetLayout>& shadowBlitSetLayout);
    };
}
