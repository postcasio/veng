#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class Image;
    class ImageView;

    /// @brief Fullscreen SSAO pass that estimates occlusion with a 16-sample hemisphere kernel
    ///        and writes a single-channel AO factor to its own full-res R8 target.
    ///
    /// Samples the g-buffer depth and world-normal handles from PassIO, works in view space
    /// (reconstructing view-space position from depth), and writes an R8Unorm occlusion factor.
    ///
    /// The pass owns its AO target (ColorAttachment | Sampled): a downstream lighting pass
    /// samples it through bindless, so it is an Imported resource, not a graph transient
    /// (a transient exposes no Ref<ImageView> to register). GetAoView / GetAoHandle are the
    /// accessors the renderer uses to fill PassIO.Ssao / PassIO.SsaoHandle.
    class SsaoScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and allocates the AO target at the given extent.
        SsaoScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                      SamplerHandle samplerHandle, uvec2 extent);
        ~SsaoScenePass() override;

        SsaoScenePass(const SsaoScenePass&) = delete;
        SsaoScenePass& operator=(const SsaoScenePass&) = delete;

        /// @brief Reallocates the AO target at the new extent.
        void Resize(uvec2 extent) override;

        /// @brief Contributes the fullscreen SSAO pass into the graph, writing the AO target.
        void Declare(RenderGraph& graph, const PassIO& io) override;

        /// @brief The produced AO target view; the renderer binds this per Execute.
        [[nodiscard]] const Ref<ImageView>& GetAoView() const { return m_AoView; }
        /// @brief The bindless handle for the AO target; threaded to the lighting pass through PassIO.
        [[nodiscard]] TextureHandle GetAoHandle() const { return m_AoHandle; }

    private:
        /// @brief Allocates or reallocates the AO target at the current extent and re-registers it.
        void CreateTarget();

        Context& m_Context;
        Ref<GraphicsPipeline> m_Pipeline;
        SamplerHandle m_SamplerHandle;
        uvec2 m_Extent;

        Ref<Image> m_AoImage;
        Ref<ImageView> m_AoView;
        TextureHandle m_AoHandle;
    };
}
