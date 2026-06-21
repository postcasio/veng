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

/// @brief Reusable pipeline-stage layer that SceneRenderer composes its pipeline from.
///
/// A ScenePass is a self-contained stage: it knows how to size its own resources,
/// declare its reads/writes, and record — never what feeds it. The renderer owns the
/// wiring (which pass reads whose target); each pass owns itself.
///
/// A ScenePass is distinct from RenderGraph::Pass: it contributes one or more
/// RenderGraph passes into the renderer's single internal graph; it is not one.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class GraphicsPipeline;

    /// @brief Record-time context handed to a ScenePass callback.
    ///
    /// Wraps the RenderGraph PassContext and adds a typed View() accessor that reads
    /// this frame's SceneView from the graph's opaque user pointer. SceneRenderer sets
    /// the pointer on every Execute.
    class ScenePassContext
    {
    public:
        /// @brief The command buffer for this pass.
        [[nodiscard]] CommandBuffer& Cmd() const { return m_Inner.Cmd(); }

        /// @brief This frame's scene view.
        ///
        /// Asserts the graph's user pointer is non-null, then reinterprets it.
        /// SceneRenderer sets it to &view on every Execute.
        [[nodiscard]] const SceneView& View() const
        {
            const void* userData = m_Inner.UserData();
            VE_ASSERT(userData != nullptr,
                      "ScenePassContext::View: the RenderGraph user pointer is null — "
                      "a ScenePass was replayed outside SceneRenderer::Execute");
            return *static_cast<const SceneView*>(userData);
        }

        /// @brief Resolves a declared transient to its concrete view for this frame.
        [[nodiscard]] ImageView& Resolved(ResourceId id) const { return m_Inner.Resolved(id); }

    private:
        /// @brief Constructed via ScenePass::Wrap; SceneRenderer guarantees the pointer is set.
        friend class ScenePass;
        explicit ScenePassContext(PassContext& inner) : m_Inner(inner) {}

        /// @brief The underlying RenderGraph PassContext.
        PassContext& m_Inner;
    };

    /// @brief Named-slot wiring the SceneRenderer hands each pass.
    ///
    /// The renderer fills the slots a given pass reads or writes; resources flow both
    /// directions (a pass may produce one a later pass consumes), and a pass may
    /// declare multiple RenderGraph passes.
    ///
    /// The renderer owns the g-buffer/HDR/output images and Imports them into the
    /// graph; the ids name those imports and the handles name the bindless slots a
    /// sampling pass reads through.
    struct PassIO
    {
        /// @brief G0 — base color; written by the geometry pass.
        ResourceId GBufferAlbedo;
        /// @brief G1 — world-space normal.
        ResourceId GBufferNormal;
        /// @brief G2 — packed occlusion/roughness/metallic/emissive.
        ResourceId GBufferOrm;
        /// @brief Depth attachment, also a sampled source for the lighting pass.
        ResourceId GBufferDepth;

        /// @brief Bindless texture slots for the g-buffer images, threaded to sampling passes.
        TextureHandle AlbedoHandle;
        /// @brief Bindless slot for the world-normal target.
        TextureHandle NormalHandle;
        /// @brief Bindless slot for the packed ORM target.
        TextureHandle OrmHandle;
        /// @brief Bindless slot for the depth target.
        TextureHandle DepthHandle;

        /// @brief HDR target the lighting pass writes and the tail pass samples.
        ResourceId Hdr;
        /// @brief Bindless slot for the HDR target.
        TextureHandle HdrHandle;

        /// @brief Screen-space AO target produced by SsaoScenePass and sampled by the lighting pass.
        ///
        /// Valid only when the AO pass is wired (Settings.AO). An invalid handle means no AO
        /// pass is active.
        ResourceId Ssao;
        /// @brief Bindless slot for the SSAO target; invalid when AO is off.
        TextureHandle SsaoHandle;

        /// @brief Bloom pyramid mip 0 after the up-sweep, blitted by the Bloom debug arm.
        ///
        /// Valid only when the bloom sweep is wired (Final + Settings.Bloom, or the Bloom debug
        /// mode). The debug blit declares .Sample so the graph derives the General → ShaderReadOnly
        /// transition after the up-sweep writes mip 0. An invalid handle means no bloom sweep is active.
        ResourceId BloomMip0;
        /// @brief Bindless slot for the bloom pyramid mip 0 view; invalid when bloom is off.
        TextureHandle BloomMip0Handle;

        /// @brief Shared sampler bindless slot used by fullscreen passes.
        SamplerHandle SamplerHandle;

        /// @brief Directional shadow atlas written by ShadowScenePass and sampled by the lighting pass.
        ///
        /// Invalid when Settings.Shadows is false. When valid the lighting pass declares .Sample
        /// so the graph derives the DepthAttachment → ShaderReadOnly transition. The atlas
        /// reaches the lighting pass through a dedicated descriptor set (ShadowView), not bindless.
        ResourceId ShadowMap;

        /// @brief Directional shadow atlas view bound into the lighting pass's dedicated descriptor set.
        ///
        /// The bound-view seam: a producer's Ref<ImageView> bound into a consumer's own set, not a
        /// bindless slot. Null when the shadow pass is compiled out. Always sampled in ShaderReadOnly
        /// layout after the graph-derived transition.
        Ref<ImageView> ShadowView;

        /// @brief Punctual shadow atlas written by PunctualShadowScenePass and sampled by the lighting pass.
        ///
        /// Invalid when the punctual shadow pass is compiled out. When valid the lighting pass declares
        /// .Sample so the graph derives the DepthAttachment → ShaderReadOnly transition. The atlas is
        /// renderer-owned (set 1 binding 4, off bindless); the punctual pass writes the view threaded
        /// in PunctualShadowView.
        ResourceId PunctualShadowMap;
        /// @brief Punctual shadow atlas view for the lighting pass's set 1 binding.
        Ref<ImageView> PunctualShadowView;

        /// @brief Imported output id the terminal pass writes.
        ResourceId Output;
    };

    /// @brief Abstract base for a reusable, self-contained SceneRenderer pipeline stage.
    ///
    /// The renderer calls Configure/Resize when those knobs change, then Declare to
    /// contribute the pass's RenderGraph pass(es) and record callback into the
    /// renderer's single internal graph.
    class ScenePass
    {
    public:
        /// @brief Virtual destructor; subclasses own their resources.
        virtual ~ScenePass() = default;

        /// @brief Notifies the pass of a topology/settings change.
        virtual void Configure(const SceneRendererSettings&) {}
        /// @brief Notifies the pass of a render-target extent change.
        virtual void Resize(uvec2) {}
        /// @brief Contributes this pass's RenderGraph pass(es) into graph.
        /// @param graph  The renderer's internal graph being rebuilt.
        /// @param io     Named resource slots the renderer has populated.
        virtual void Declare(RenderGraph& graph, const PassIO& io) = 0;

    protected:
        /// @brief Types a RenderGraph PassContext as a ScenePassContext.
        ///
        /// A subclass calls this inside its Declare record callback to access the
        /// per-frame SceneView via the typed View() accessor.
        [[nodiscard]] static ScenePassContext Wrap(PassContext& inner)
        {
            return ScenePassContext(inner);
        }
    };

    /// @brief Primary runtime-bound input for a PostProcessScenePass.
    ///
    /// Names the imported upstream target this pass samples and the material fields
    /// the pass writes each frame with the resolved bindless handles.
    struct PostProcessInput
    {
        /// @brief The imported upstream target (declared .Sample so the graph derives the barrier).
        ResourceId Source;
        /// @brief Bindless slot for the source view, written into TextureField each frame.
        TextureHandle SourceTexture;
        /// @brief Bindless slot for the shared sampler, written into SamplerField each frame.
        SamplerHandle Sampler;
        /// @brief Material field name to receive the source texture handle.
        string TextureField;
        /// @brief Material field name to receive the sampler handle.
        string SamplerField;
    };

    /// @brief Optional second runtime-bound input for a PostProcessScenePass.
    ///
    /// Some PostProcess materials sample a second upstream target alongside the
    /// primary one (e.g. the bloom composite samples the HDR target and the blurred
    /// bloom residual together). When Texture is valid the pass declares .Sample on
    /// Source and writes the handle pair into the named fields each frame; an invalid
    /// Texture is skipped.
    struct PostProcessExtraInput
    {
        /// @brief The second imported upstream target.
        ResourceId Source;
        /// @brief Bindless slot for the second source; invalid means this entry is inactive.
        TextureHandle Texture;
        /// @brief Bindless sampler slot written into SamplerField each frame.
        SamplerHandle Sampler;
        /// @brief Material field name to receive the second texture handle.
        string TextureField;
        /// @brief Material field name to receive the second sampler handle.
        string SamplerField;
    };

    /// @brief Fullscreen post-process pass driven by a PostProcess-domain material.
    ///
    /// Builds a fullscreen GraphicsPipeline from the material's reflected shaders and
    /// pipeline layout against a renderer-supplied single-color target format, binds
    /// set-0 bindless, samples one upstream imported target through a runtime-bound
    /// material handle field (written each frame via Material::SetTextureHandle /
    /// SetSamplerHandle), pushes the material's per-draw selector at the PostProcess
    /// domain's offset (0), and draws the fullscreen triangle.
    ///
    /// The renderer owns the wiring: the constructor names which imported source the
    /// pass samples, the bindless slots for that source, the material field names to
    /// write them into, and the output id/format. One PostProcessScenePass type drives
    /// any PostProcess material.
    class PostProcessScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass and builds the fullscreen pipeline from the material.
        /// @param context       Renderer context for pipeline and resource creation.
        /// @param material      The PostProcess-domain material driving this pass.
        /// @param input         Primary runtime-bound input descriptor.
        /// @param output        The output ResourceId this pass writes.
        /// @param outputFormat  Color format of the output target.
        /// @param extent        Initial render extent; updated via Resize.
        PostProcessScenePass(Context& context, AssetHandle<Material> material,
                             PostProcessInput input, ResourceId output, Format outputFormat,
                             uvec2 extent);

        /// @brief Registers a second runtime-bound input sampled alongside the primary.
        ///
        /// The graph derives the second source's attachment → shader-read barrier from
        /// its declared .Sample, exactly as for the primary input.
        void SetExtraInput(PostProcessExtraInput extra) { m_Extra = std::move(extra); }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the fullscreen pass into graph and sets its record callback.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Builds or rebuilds the fullscreen GraphicsPipeline from the material.
        void BuildPipeline();

        /// @brief Context for pipeline creation.
        Context& m_Context;
        /// @brief The PostProcess material driving this pass.
        AssetHandle<Material> m_Material;
        /// @brief Primary input descriptor.
        PostProcessInput m_Input;
        /// @brief Optional second input (inactive when Texture is invalid).
        PostProcessExtraInput m_Extra;
        /// @brief Output resource id.
        ResourceId m_Output;
        /// @brief Output color format.
        Format m_OutputFormat;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Built from the material's shaders.
        Ref<GraphicsPipeline> m_Pipeline;
    };
}
