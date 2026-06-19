#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
    class Shader;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class Image;
    class PipelineLayout;

    // The depth-only directional-shadow pass. It owns a square depth target
    // (D32Sfloat, DepthAttachment | Sampled) rendered from the directional
    // light's orthographic view, and contributes one depth-only RenderGraph pass
    // drawing the scene's opaque meshes with the light-space MVP. The produced
    // shadow map is registered into bindless once (re-registered on recreate) and
    // its handle/id reach the lighting pass through PassIO; the light-space matrix
    // rides the view-constants buffer, written by the renderer per Execute.
    //
    // The target is single-copy and consumed within the renderer's internal graph;
    // the graph derives its write→sample barriers from the lighting pass's declared
    // .Sample(ShadowMap). On recreate (Configure/Resize) the old image retires and
    // its bindless slot releases through the deferred per-frame window, so an
    // in-flight frame's sample is never reclaimed early.
    class ShadowScenePass final : public ScenePass
    {
    public:
        ShadowScenePass(Context& context, AssetManager& assets, u32 resolution);
        ~ShadowScenePass() override;

        // The shadow map's bindless slot and imported id, threaded into PassIO so
        // the lighting pass declares its sample and reads the handle.
        [[nodiscard]] TextureHandle GetShadowHandle() const { return m_ShadowHandle; }
        [[nodiscard]] const Ref<ImageView>& GetShadowView() const { return m_ShadowView; }
        [[nodiscard]] u32 GetResolution() const { return m_Resolution; }

        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        // Recreate the depth target at the current resolution and (re-)register it
        // into bindless, releasing the old slot through the deferred window.
        void CreateTarget();

        Context& m_Context;
        u32 m_Resolution;

        Ref<Image> m_ShadowImage;
        Ref<ImageView> m_ShadowView;
        TextureHandle m_ShadowHandle;

        Ref<GraphicsPipeline> m_Pipeline;
        Ref<PipelineLayout> m_Layout;
        AssetHandle<Veng::Shader> m_VertexShader;
    };
}
