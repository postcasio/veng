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

    // The screen-space ambient occlusion pass: a fullscreen ScenePass that samples
    // the g-buffer depth + world-normal handles (received through PassIO), works in
    // view space (reconstructing view-space position from depth and carrying the
    // normal into view space), estimates occlusion with a fixed 16-sample hemisphere
    // kernel, and writes a single-channel AO factor to its own full-res R8 target.
    //
    // The pass OWNS that AO target (an R8Unorm ColorAttachment | Sampled image): a
    // downstream lighting pass samples it through bindless, so it is a renderer-style
    // Imported resource, not a graph transient (a transient exposes no Ref<ImageView>
    // to register). It recreates the target on Resize/Configure through the deferred
    // Release() retire path and re-registers it. The produced id + bindless handle
    // flow to the lighting pass through PassIO.Ssao / PassIO.SsaoHandle, which the
    // renderer fills from this pass's accessors.
    class SsaoScenePass final : public ScenePass
    {
    public:
        SsaoScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                      SamplerHandle samplerHandle, uvec2 extent);
        ~SsaoScenePass() override;

        SsaoScenePass(const SsaoScenePass&) = delete;
        SsaoScenePass& operator=(const SsaoScenePass&) = delete;

        void Resize(uvec2 extent) override;
        void Declare(RenderGraph& graph, const PassIO& io) override;

        // The produced AO target: the renderer Imports the id, binds this view per
        // Execute, and threads the handle to the lighting pass through PassIO.
        [[nodiscard]] const Ref<ImageView>& GetAoView() const { return m_AoView; }
        [[nodiscard]] TextureHandle GetAoHandle() const { return m_AoHandle; }

    private:
        // Recreate the owned AO target at the current extent and (re-)register it,
        // releasing the prior slot through the deferred per-frame retire window.
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
