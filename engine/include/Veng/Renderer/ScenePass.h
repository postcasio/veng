#pragma once

#include <string>

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class Material;
}

// The reusable-pass-unit layer SceneRenderer composes its pipeline from. A
// ScenePass is a self-contained pipeline stage: it knows how to size its own
// resources, declare its reads/writes, and record — never what feeds it. The
// renderer owns the wiring (which pass reads whose target); each pass owns itself.
//
// A ScenePass is distinct from RenderGraph::Pass: it contributes one or more
// RenderGraph passes into the renderer's single internal graph, it is not one.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class GraphicsPipeline;

    // The record-time context a ScenePass callback receives: the RenderGraph
    // record-time channel plus this frame's SceneView, typed back from the graph's
    // opaque user pointer. SceneRenderer is the Scene-aware layer that owns this
    // wrapper and guarantees the pointer is set and the right type on every Execute.
    class ScenePassContext
    {
    public:
        [[nodiscard]] CommandBuffer& Cmd() const { return m_Inner.Cmd(); }

        // This frame's view. Asserts the graph's user pointer is non-null, then
        // reinterprets it — SceneRenderer sets it to &view on every Execute.
        [[nodiscard]] const SceneView& View() const
        {
            void* userData = m_Inner.UserData();
            VE_ASSERT(userData != nullptr,
                      "ScenePassContext::View: the RenderGraph user pointer is null — "
                      "a ScenePass was replayed outside SceneRenderer::Execute");
            return *static_cast<const SceneView*>(userData);
        }

        [[nodiscard]] ImageView& Resolved(ResourceId id) const { return m_Inner.Resolved(id); }

    private:
        // A ScenePass wraps the RenderGraph PassContext it receives in its Declare
        // callback through ScenePass::Wrap; SceneRenderer guarantees the user pointer
        // is set on every Execute.
        friend class ScenePass;
        explicit ScenePassContext(PassContext& inner) : m_Inner(inner) {}

        PassContext& m_Inner;
    };

    // The wiring a SceneRenderer hands each pass: a named-slot struct, not a flat
    // in/out pair. The renderer fills the slots a given pass reads/writes; resources
    // flow both directions (a pass may produce one a later pass consumes), and a
    // pass may declare multiple RenderGraph passes.
    //
    // The renderer owns the g-buffer/HDR/output images and Imports them into the
    // graph; the ids name those imports and the handles name the bindless slots a
    // sampling pass reads through. The geometry pass writes the g-buffer; a
    // fullscreen pass reads it (by handle) and writes the output. The HDR slot is
    // the lighting pass's target.
    struct PassIO
    {
        // The g-buffer the geometry pass writes and a fullscreen pass samples.
        ResourceId GBufferAlbedo;  // G0 — base color
        ResourceId GBufferNormal;  // G1 — world-space normal
        ResourceId GBufferOrm;     // G2 — packed occlusion/roughness/metallic/emissive
        ResourceId GBufferDepth;   // depth attachment, also a sampled source

        // The bindless texture slots the renderer registered for the g-buffer
        // images, threaded to whichever pass samples them.
        TextureHandle AlbedoHandle;
        TextureHandle NormalHandle;
        TextureHandle OrmHandle;
        TextureHandle DepthHandle;

        // The HDR target a lighting pass writes (and a tail pass samples).
        ResourceId Hdr;
        TextureHandle HdrHandle;

        // The screen-space AO target the SsaoScenePass produces and the lighting
        // pass samples — valid only when the AO pass is wired (Settings.AO). The id
        // is the SsaoScenePass's own Imported target; the handle is its bindless
        // slot. An invalid handle means no AO pass this build.
        ResourceId Ssao;
        TextureHandle SsaoHandle;

        // The shared sampler bindless slot a fullscreen pass samples through.
        SamplerHandle SamplerHandle;

        // The directional shadow map the ShadowScenePass writes (depth) and the
        // lighting pass samples. The id is invalid and the handle unbound when the
        // shadow pass is compiled out (Settings.Shadows == false); the lighting pass
        // declares its .Sample only when the id is valid and reads full visibility
        // when the handle is unbound.
        ResourceId ShadowMap;
        TextureHandle ShadowHandle;

        // The imported output id the terminal pass writes.
        ResourceId Output;
    };

    // A reusable, self-contained pipeline stage. The renderer calls Configure/Resize
    // when those knobs change, then Declare to contribute the pass's RenderGraph
    // pass(es) + record callback into the renderer's single internal graph.
    class ScenePass
    {
    public:
        virtual ~ScenePass() = default;

        virtual void Configure(const SceneRendererSettings&) {}
        virtual void Resize(uvec2) {}
        virtual void Declare(RenderGraph& graph, const PassIO& io) = 0;

    protected:
        // Type a RenderGraph record-time context as a ScenePassContext. A subclass
        // calls this inside its Declare record callback to read the per-frame
        // SceneView.
        [[nodiscard]] static ScenePassContext Wrap(PassContext& inner)
        {
            return ScenePassContext(inner);
        }
    };

    // A reusable fullscreen post-process pass driven by a PostProcess-domain
    // material. It builds a fullscreen GraphicsPipeline from the material's
    // reflected shaders + pipeline layout against a single color target whose
    // format the renderer supplies (a PostProcess material's loader cannot know
    // it), binds set-0 bindless, samples one upstream Imported target through a
    // runtime-bound material handle field (its bindless index written each frame
    // via Material::SetTextureHandle/SetSamplerHandle), pushes the material's
    // per-draw selector at the PostProcess domain's offset (0), and draws the
    // fullscreen triangle.
    //
    // The renderer owns the wiring: the constructor names which Imported source
    // the pass samples, the bindless slots for that source, the material field
    // names to write them into, and the output id/format. One PostProcessScenePass
    // type drives any PostProcess material; a chain of them is the post-stack the
    // renderer lists.
    struct PostProcessInput
    {
        // The Imported upstream target this pass samples (declared .Sample so the
        // graph derives its attachment → shader-read barrier).
        ResourceId Source;
        // The bindless slots the renderer registered for the source view + the
        // shared sampler, written into the material's input handle fields each
        // frame.
        TextureHandle SourceTexture;
        SamplerHandle Sampler;
        // The material's runtime-bound input field names (the texture handle field
        // and its paired sampler handle field).
        string TextureField;
        string SamplerField;
    };

    // A second runtime-bound input some PostProcess materials sample alongside the
    // primary one (the bloom composite samples the HDR target and the blurred bloom
    // residual together). When Texture is valid the pass declares .Sample on Source
    // and writes the handle pair into the named fields each frame; an
    // invalid-handle entry is skipped.
    struct PostProcessExtraInput
    {
        ResourceId Source;
        TextureHandle Texture;
        SamplerHandle Sampler;
        string TextureField;
        string SamplerField;
    };

    class PostProcessScenePass final : public ScenePass
    {
    public:
        PostProcessScenePass(Context& context, AssetHandle<Material> material,
                             PostProcessInput input, ResourceId output,
                             Format outputFormat, uvec2 extent);

        // A pass sampling a second runtime-bound source (the bloom composite reads
        // the HDR target and the blurred bloom together). The graph derives the
        // second source's attachment → shader-read barrier from its declared
        // .Sample, exactly as for the primary input.
        void SetExtraInput(PostProcessExtraInput extra) { m_Extra = std::move(extra); }

        void Resize(uvec2 extent) override { m_Extent = extent; }
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        void BuildPipeline();

        Context& m_Context;
        AssetHandle<Material> m_Material;
        PostProcessInput m_Input;
        PostProcessExtraInput m_Extra;
        ResourceId m_Output;
        Format m_OutputFormat;
        uvec2 m_Extent;
        Ref<GraphicsPipeline> m_Pipeline;
    };
}
