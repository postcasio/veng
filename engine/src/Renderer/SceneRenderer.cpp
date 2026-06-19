#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>

#include <fmt/format.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Assert.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's fullscreen shaders, addressed by their hardcoded
        // core-pack ids (the AssetManager auto-mounts the core pack).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId DeferredLightingFragId{0x6569EBAC0810CC1FULL};
        constexpr AssetId TonemapMaterialId{0xBC968C8771B00434ULL};
        constexpr AssetId AlbedoBlitFragId{0xF90F709155D04BE7ULL};
        constexpr AssetId NormalBlitFragId{0x5A2CD7B270EAE5CDULL};
        constexpr AssetId DepthBlitFragId{0xE05F5F86E72F96D5ULL};

        // The HDR lighting target's format: a linear signed-float color target the
        // lighting pass writes and the tail pass samples. Same format G1 already
        // uses as a sampled color target, so a color-attachment + sampled RGBA16F
        // is established on the platform.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // The shared push-constant block both brick stages declare: the vertex
        // stage's MVP (offset 0) and the per-draw MaterialIndex (offset 64,
        // pushed by Material::Bind) and NormalMatrix (offset 80). The renderer
        // pushes MVP and NormalMatrix; Material::Bind pushes the selector in
        // between. NormalMatrix is a column-major float3x3 — three columns padded
        // to 16 bytes each (the std140 / Slang-column-major matrix layout).
        struct MeshPushConstants
        {
            mat4 MVP;
        };

        constexpr u32 NormalMatrixPushOffset = 80;

        struct NormalMatrixPush
        {
            vec4 Column0;
            vec4 Column1;
            vec4 Column2;
        };

        static_assert(sizeof(NormalMatrixPush) == 48, "NormalMatrix push must be a column-major float3x3 (48 bytes)");

        // The deferred-lighting fragment shader's push block: the bindless slots it
        // samples the g-buffer through, plus the directional light. Matches the
        // shader's PushConstants byte-for-byte (the four u32 indices are packed,
        // then two vec4s at their 16-byte alignment).
        struct LightingPushConstants
        {
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 DepthTexture;
            u32 Sampler;
            vec4 LightDirection; // xyz: world-space travel direction
            vec4 LightColor;     // rgb: linear color; a: intensity
        };

        // The shared debug-blit fragment push block: the bindless slots a fullscreen
        // blit samples one g-buffer channel through. The albedo, normal, and depth
        // visualizers all declare exactly these two fields.
        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };

        // The deferred g-buffer geometry pass: draws every (Transform, MeshRenderer)
        // entity into the renderer's imported g-buffer (G0 albedo, G1 world-normal)
        // with the shared depth attachment. Each material's pipeline writes both
        // color targets through its GBufferOutput. Sourced from the per-frame
        // SceneView.
        class GBufferScenePass final : public ScenePass
        {
        public:
            GBufferScenePass(Context& context, uvec2 extent)
                : m_Context(context), m_Extent(extent)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                graph.AddPass("Scene GBuffer")
                    .Color({
                        .Resource = io.GBufferAlbedo,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.05f, 0.05f, 0.08f, 1.0f},
                    })
                    .Color({
                        .Resource = io.GBufferNormal,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
                    })
                    .Depth({
                        .Resource = io.GBufferDepth,
                        .Load = LoadOp::Clear,
                        // Stored: the lighting pass reads depth as a texture.
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{1.0f, 0},
                    })
                    .Execute([this](PassContext& inner)
                    {
                        Record(Wrap(inner));
                    });
            }

        private:
            void Record(const ScenePassContext& ctx) const
            {
                CommandBuffer& cmd = ctx.Cmd();
                const SceneView& view = ctx.View();

                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);

                const mat4 viewProjection = view.Camera.ViewProjection();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                // Each (Transform, MeshRenderer) entity draws its own mesh at its
                // world transform; the mesh is the MeshRenderer's AssetHandle —
                // cooked or adopted runtime primitive alike. Scene::Each yields
                // mutable component references and the borrowed view is const; this
                // pass only reads, so casting away const to iterate is sound.
                const_cast<Scene&>(view.World).Each<Transform, MeshRenderer>(
                    [&](const Entity entity, Transform&, MeshRenderer& meshRenderer)
                {
                    if (!meshRenderer.Mesh.IsLoaded())
                        return;

                    const Mesh& mesh = *meshRenderer.Mesh.Get();
                    const std::span<const AssetHandle<Material>> materials = mesh.GetMaterials();

                    bool materialsReady = true;
                    for (const AssetHandle<Material>& material : materials)
                        materialsReady = materialsReady && material.IsLoaded();
                    if (!materialsReady)
                        return;

                    cmd.BindVertexBuffer(mesh.GetVertexBuffer());
                    cmd.BindIndexBuffer(mesh.GetIndexBuffer());

                    const mat4 world = WorldMatrix(view.World, entity);
                    const mat4 mvp = viewProjection * world;

                    // The normal matrix is the inverse-transpose of the model's
                    // upper 3x3 — correct under non-uniform scale. Packed
                    // column-major into three 16-byte-aligned columns matching the
                    // shader's column-major float3x3.
                    const mat3 normalMatrix = glm::inverseTranspose(mat3(world));
                    const NormalMatrixPush normalPush{
                        .Column0 = vec4(normalMatrix[0], 0.0f),
                        .Column1 = vec4(normalMatrix[1], 0.0f),
                        .Column2 = vec4(normalMatrix[2], 0.0f),
                    };

                    for (const SubMesh& subMesh : mesh.GetSubMeshes())
                    {
                        if (subMesh.MaterialIndex == SubMesh::NoMaterial)
                            continue;

                        // The submesh's material binds its pipeline (and pushes its
                        // per-draw index selector) first; the bindless registry then
                        // binds set 0 into that pipeline's layout — Bind uses the
                        // currently-bound layout, so the pipeline must be bound
                        // before it. The MVP occupies the leading 64 bytes of the
                        // shared push block, the NormalMatrix offset 80;
                        // Material::Bind already pushed MaterialIndex at offset 64.
                        materials[subMesh.MaterialIndex].Get()->Bind(cmd);
                        registry.Bind(cmd);
                        cmd.PushConstants(MeshPushConstants{.MVP = mvp});
                        cmd.PushConstants(normalPush, NormalMatrixPushOffset);

                        cmd.DrawIndexed(subMesh.IndexCount, 1, subMesh.IndexOffset, 0, 0);
                    }
                });
            }

            Context& m_Context;
            uvec2 m_Extent;
        };

        // The fullscreen deferred-lighting pass: samples the g-buffer (G0 albedo,
        // G1 world-normal, depth) through their bindless handles, applies the
        // scene's directional light from the per-frame SceneView, and writes the
        // renderer's imported HDR target. Declaring .Sample on each g-buffer id
        // lets the graph derive the attachment → shader-read transitions —
        // including the depth attachment → shader-read transition, the only depth
        // target read as a texture in the engine.
        class DeferredLightingScenePass final : public ScenePass
        {
        public:
            DeferredLightingScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const TextureHandle albedoHandle = io.AlbedoHandle;
                const TextureHandle normalHandle = io.NormalHandle;
                const TextureHandle depthHandle = io.DepthHandle;
                const SamplerHandle samplerHandle = io.SamplerHandle;

                graph.AddPass("Deferred Lighting")
                    .Color({
                        .Resource = io.Hdr,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                    })
                    .Sample(io.GBufferAlbedo)
                    .Sample(io.GBufferNormal)
                    .Sample(io.GBufferDepth)
                    .Execute([this, albedoHandle, normalHandle, depthHandle, samplerHandle](PassContext& inner)
                    {
                        const ScenePassContext ctx = Wrap(inner);
                        CommandBuffer& cmd = ctx.Cmd();
                        const Light& light = ctx.View().Light;

                        cmd.BindPipeline(m_Pipeline);
                        cmd.SetViewport({0, 0}, m_Extent);
                        cmd.SetScissor({0, 0}, m_Extent);
                        m_Context.GetBindlessRegistry().Bind(cmd);
                        cmd.PushConstants(LightingPushConstants{
                            .AlbedoTexture = albedoHandle.Index,
                            .NormalTexture = normalHandle.Index,
                            .DepthTexture = depthHandle.Index,
                            .Sampler = samplerHandle.Index,
                            .LightDirection = vec4(light.Direction, 0.0f),
                            .LightColor = vec4(light.Color, light.Intensity),
                        });
                        cmd.DrawFullscreenTriangle();
                    });
            }

        private:
            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
        };

        // A reusable fullscreen blit pass: samples one g-buffer channel (selected by
        // the PassIO handle the constructor names) through bindless and writes the
        // renderer's imported output. It is the shared shape behind the Albedo,
        // Normal, and Depth debug views — each constructs one with the matching
        // pipeline (the channel-specific fragment shader does the decode) and selects
        // its source via a small accessor. The source id it declares .Sample on lets
        // the graph derive the channel's attachment → shader-read transition.
        class FullscreenBlitScenePass final : public ScenePass
        {
        public:
            // Selects which g-buffer channel this blit reads from the PassIO.
            enum class Source { Albedo, Normal, Depth };

            FullscreenBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent, Source source)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent), m_Source(source)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const ResourceId sourceId = SourceId(io);
                const TextureHandle textureHandle = SourceHandle(io);
                const SamplerHandle samplerHandle = io.SamplerHandle;

                graph.AddPass("Debug Blit")
                    .Color({
                        .Resource = io.Output,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                    })
                    .Sample(sourceId)
                    .Execute([this, textureHandle, samplerHandle](PassContext& inner)
                    {
                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(m_Pipeline);
                        cmd.SetViewport({0, 0}, m_Extent);
                        cmd.SetScissor({0, 0}, m_Extent);
                        m_Context.GetBindlessRegistry().Bind(cmd);
                        cmd.PushConstants(BlitPushConstants{
                            .Texture = textureHandle.Index,
                            .Sampler = samplerHandle.Index,
                        });
                        cmd.DrawFullscreenTriangle();
                    });
            }

        private:
            [[nodiscard]] ResourceId SourceId(const PassIO& io) const
            {
                switch (m_Source)
                {
                    case Source::Albedo: return io.GBufferAlbedo;
                    case Source::Normal: return io.GBufferNormal;
                    case Source::Depth:  return io.GBufferDepth;
                }
                VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
            }

            [[nodiscard]] TextureHandle SourceHandle(const PassIO& io) const
            {
                switch (m_Source)
                {
                    case Source::Albedo: return io.AlbedoHandle;
                    case Source::Normal: return io.NormalHandle;
                    case Source::Depth:  return io.DepthHandle;
                }
                VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
            }

            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
            Source m_Source;
        };
    }

    PostProcessScenePass::PostProcessScenePass(
        Context& context, AssetHandle<Material> material, PostProcessInput input,
        ResourceId output, Format outputFormat, uvec2 extent)
        : m_Context(context), m_Material(std::move(material)), m_Input(std::move(input)),
          m_Output(output), m_OutputFormat(outputFormat), m_Extent(extent)
    {
    }

    void PostProcessScenePass::BuildPipeline()
    {
        VE_ASSERT(m_Material.IsLoaded(),
                  "PostProcessScenePass: the PostProcess material is not resident");

        const Material& material = *m_Material.Get();
        VE_ASSERT(material.GetDomain() == MaterialDomain::PostProcess,
                  "PostProcessScenePass: material '{}' is not a PostProcess material", material.GetName());

        // The fullscreen shape: the material's screenspace vertex stage, its
        // fragment stage, no vertex inputs, no depth, one color target the renderer
        // supplies the format for. The layout (set 0 reserved, the selector push
        // range) comes from the loader; only this color-format-dependent
        // GraphicsPipeline::Create is the pass's to make.
        m_Pipeline = GraphicsPipeline::Create(m_Context, {
            .Name = fmt::format("PostProcess Pipeline ({})", material.GetName()),
            .ColorAttachments = {{.Format = m_OutputFormat}},
            .PipelineLayout = material.GetPipelineLayout(),
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex,   .Module = material.GetVertexModule()},
                {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
            },
        });
    }

    void PostProcessScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        if (!m_Pipeline)
            BuildPipeline();

        const PostProcessInput input = m_Input;

        graph.AddPass("PostProcess")
            .Color({
                .Resource = m_Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(input.Source)
            .Execute([this, input](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                // The per-frame handle write mutates the material's parameter
                // block; AssetHandle::Get is const, so cast away const to write.
                Material& material = const_cast<Material&>(*m_Material.Get());

                // Bind the live upstream bindless slots into the material's
                // runtime-bound input handle fields. The write lands in the
                // ring-buffered block's current frame region (no stall, no hazard);
                // it must precede Material::Bind so the pushed selector reads this
                // frame's region.
                material.SetTextureHandle(input.TextureField, input.SourceTexture);
                material.SetSamplerHandle(input.SamplerField, input.Sampler);

                // Bind the pass's pipeline first (the bindless registry binds set 0
                // into the currently-bound layout), then set 0, then push the
                // material selector at the PostProcess domain's offset.
                cmd.BindPipeline(m_Pipeline);
                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);
                m_Context.GetBindlessRegistry().Bind(cmd);
                material.Bind(cmd);
                cmd.DrawFullscreenTriangle();
            });
    }

    // Holds the compiled graph + the stable import bindings rebound per Execute.
    // Kept out of the header so SceneRenderer.h needs no CompiledGraph definition.
    struct SceneRenderer::Internal
    {
        Unique<CompiledGraph> Graph;
    };

    Unique<SceneRenderer> SceneRenderer::Create(const SceneRendererInfo& info)
    {
        return Unique<SceneRenderer>(new SceneRenderer(info));
    }

    SceneRenderer::SceneRenderer(const SceneRendererInfo& info)
        : m_Context(info.Context),
          m_Assets(info.Assets),
          m_OutputFormat(info.OutputFormat),
          m_Extent(info.Extent),
          m_Settings(info.Settings),
          m_Internal(CreateUnique<Internal>())
    {
        CreatePipelines();

        CreateOutput();
        CreateGBuffer();
        CreateHdr();
        Rebuild();
    }

    SceneRenderer::~SceneRenderer()
    {
        // Release the bindless slots through the per-frame retire window before the
        // images they name retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_HdrHandle);
        bindless.Release(m_SamplerHandle);
    }

    void SceneRenderer::CreatePipelines()
    {
        auto LoadShader = [this](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = m_Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what, result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> lightingFs = LoadShader(DeferredLightingFragId, "deferred-lighting fragment");
        const AssetHandle<Veng::Shader> albedoBlitFs = LoadShader(AlbedoBlitFragId, "albedo-blit fragment");
        const AssetHandle<Veng::Shader> normalBlitFs = LoadShader(NormalBlitFragId, "normal-blit fragment");
        const AssetHandle<Veng::Shader> depthBlitFs = LoadShader(DepthBlitFragId, "depth-blit fragment");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, naming the
        // color-target format the pass writes.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs, const Format format) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(m_Context, {
                .Name = name,
                .ColorAttachments = {{.Format = format}},
                .PipelineLayout = layout,
                .ShaderStages = {
                    {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                },
            });
        };

        // The lighting pipeline writes the HDR target (linear float); the tonemap and
        // the three debug blits write the output format.
        m_LightingLayout = PipelineLayout::Create(m_Context, {
            .Name = "SceneRenderer Lighting Layout",
            .PushConstantRanges = {PushConstantRange::Of<LightingPushConstants>(ShaderStage::Fragment)},
        });
        m_LightingPipeline = MakePipeline("SceneRenderer Deferred Lighting Pipeline", m_LightingLayout, lightingFs, HdrFormat);

        // The Final chain's tail is the core tonemap PostProcess material driven by
        // a PostProcessScenePass (rather than a hardcoded pipeline): it samples the
        // HDR target through a runtime-bound handle field and exposes Exposure as an
        // authored param. Loaded resident here so the pass can build its fullscreen
        // pipeline from the material's shaders against the output format.
        const AssetResult<AssetHandle<Material>> tonemap = m_Assets.LoadSync<Material>(TonemapMaterialId);
        VE_ASSERT(tonemap.has_value(), "SceneRenderer: tonemap material load failed: {}", tonemap.error().Detail);
        m_TonemapMaterial = *tonemap;

        // The three debug blits share the BlitPushConstants layout (a texture + a
        // sampler index); only the fragment shader's decode differs.
        const PushConstantRange blitRange = PushConstantRange::Of<BlitPushConstants>(ShaderStage::Fragment);

        m_AlbedoBlitLayout = PipelineLayout::Create(m_Context, {
            .Name = "SceneRenderer Albedo Blit Layout",
            .PushConstantRanges = {blitRange},
        });
        m_AlbedoBlitPipeline = MakePipeline("SceneRenderer Albedo Blit Pipeline", m_AlbedoBlitLayout, albedoBlitFs, m_OutputFormat);

        m_NormalBlitLayout = PipelineLayout::Create(m_Context, {
            .Name = "SceneRenderer Normal Blit Layout",
            .PushConstantRanges = {blitRange},
        });
        m_NormalBlitPipeline = MakePipeline("SceneRenderer Normal Blit Pipeline", m_NormalBlitLayout, normalBlitFs, m_OutputFormat);

        m_DepthBlitLayout = PipelineLayout::Create(m_Context, {
            .Name = "SceneRenderer Depth Blit Layout",
            .PushConstantRanges = {blitRange},
        });
        m_DepthBlitPipeline = MakePipeline("SceneRenderer Depth Blit Pipeline", m_DepthBlitLayout, depthBlitFs, m_OutputFormat);
    }

    void SceneRenderer::CreateOutput()
    {
        // Sampled so a consumer/composite samples the result; TransferSrc so the
        // smoke path can Download() it. Dropping the old Refs retires them through
        // the per-frame deferred-destruction path.
        //
        // This is a single-copy output, not a ring buffer per frame-in-flight: a
        // consumer samples GetOutput() in the same frame the renderer wrote it (the
        // composite/editor-panel handoff), so an output is never read across a
        // frame-in-flight boundary and the cross-frame caching hazard a ring buffer
        // would address does not arise.
        m_OutputImage = Image::Create(m_Context, {
            .Name = "SceneRenderer Output",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = m_OutputFormat,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc,
        });

        m_OutputView = ImageView::Create(m_Context, {
            .Name = "SceneRenderer Output View",
            .Image = m_OutputImage,
        });
    }

    void SceneRenderer::CreateGBuffer()
    {
        // The g-buffer is renderer-owned (sampled downstream, so not a graph
        // transient): the geometry pass writes it, the lighting pass samples it
        // through bindless. Dropping the old Refs retires them; releasing the old
        // slots defers through the same per-frame window.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_DepthHandle);

        m_AlbedoImage = Image::Create(m_Context, {
            .Name = "SceneRenderer GBuffer Albedo",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = GBuffer::AlbedoFormat,
            .Usage = GBuffer::ColorUsage,
        });
        m_AlbedoView = ImageView::Create(m_Context, {.Name = "SceneRenderer GBuffer Albedo View", .Image = m_AlbedoImage});

        m_NormalImage = Image::Create(m_Context, {
            .Name = "SceneRenderer GBuffer Normal",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = GBuffer::NormalFormat,
            .Usage = GBuffer::ColorUsage,
        });
        m_NormalView = ImageView::Create(m_Context, {.Name = "SceneRenderer GBuffer Normal View", .Image = m_NormalImage});

        m_DepthImage = Image::Create(m_Context, {
            .Name = "SceneRenderer GBuffer Depth",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = GBuffer::DepthFormat,
            .Usage = GBuffer::DepthUsage,
        });
        m_DepthView = ImageView::Create(m_Context, {.Name = "SceneRenderer GBuffer Depth View", .Image = m_DepthImage});

        if (!m_Sampler)
        {
            m_Sampler = Sampler::Create(m_Context, {
                .Name = "SceneRenderer GBuffer Sampler",
                .AddressModeU = AddressMode::ClampToEdge,
                .AddressModeV = AddressMode::ClampToEdge,
                .AddressModeW = AddressMode::ClampToEdge,
            });
            m_SamplerHandle = bindless.Register(m_Sampler);
        }

        m_AlbedoHandle = bindless.Register(m_AlbedoView);
        m_NormalHandle = bindless.Register(m_NormalView);
        m_DepthHandle = bindless.Register(m_DepthView);
    }

    void SceneRenderer::CreateHdr()
    {
        // The HDR target is renderer-owned and imported: the lighting pass writes
        // it, the tail pass samples it through bindless. Dropping the old Ref
        // retires it; releasing the old slot defers through the same per-frame
        // window.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HdrHandle);

        m_HdrImage = Image::Create(m_Context, {
            .Name = "SceneRenderer HDR",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = HdrFormat,
            .Usage = HdrUsage,
        });
        m_HdrView = ImageView::Create(m_Context, {.Name = "SceneRenderer HDR View", .Image = m_HdrImage});

        m_HdrHandle = bindless.Register(m_HdrView);
    }

    void SceneRenderer::Rebuild()
    {
        // The geometry pass always runs first (it fills the g-buffer); Settings.Mode
        // selects the tail. Final is the full deferred chain (lighting → tonemap); the
        // debug views terminate after the g-buffer with one fullscreen blit of a single
        // channel — a real topology change driven through Configure → recompile.
        // Declare the imported ids first: the PostProcessScenePass binds the HDR
        // source and output ids at construction, so they must be resolved before
        // the pass set is built.
        RenderGraph graph(m_Context);
        const ResourceId albedoId = graph.Import("SceneRenderer GBuffer Albedo");
        const ResourceId normalId = graph.Import("SceneRenderer GBuffer Normal");
        const ResourceId depthId = graph.Import("SceneRenderer GBuffer Depth");
        const ResourceId hdrId = graph.Import("SceneRenderer HDR");
        m_OutputId = graph.Import("SceneRenderer Output");

        m_AlbedoId = albedoId;
        m_NormalId = normalId;
        m_DepthId = depthId;
        m_HdrId = hdrId;

        m_Passes.clear();
        m_Passes.push_back(CreateUnique<GBufferScenePass>(m_Context, m_Extent));
        switch (m_Settings.Mode)
        {
            case DebugView::Final:
                m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(m_Context, m_LightingPipeline, m_Extent));
                m_Passes.push_back(CreateUnique<PostProcessScenePass>(
                    m_Context, m_TonemapMaterial,
                    PostProcessInput{
                        .Source = m_HdrId,
                        .SourceTexture = m_HdrHandle,
                        .Sampler = m_SamplerHandle,
                        .TextureField = "Hdr",
                        .SamplerField = "HdrSampler",
                    },
                    m_OutputId, m_OutputFormat, m_Extent));
                break;
            case DebugView::Albedo:
                m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                    m_Context, m_AlbedoBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Albedo));
                break;
            case DebugView::Normal:
                m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                    m_Context, m_NormalBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Normal));
                break;
            case DebugView::Depth:
                m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                    m_Context, m_DepthBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Depth));
                break;
        }

        const PassIO io{
            .GBufferAlbedo = albedoId,
            .GBufferNormal = normalId,
            .GBufferDepth = depthId,
            .AlbedoHandle = m_AlbedoHandle,
            .NormalHandle = m_NormalHandle,
            .DepthHandle = m_DepthHandle,
            .Hdr = hdrId,
            .HdrHandle = m_HdrHandle,
            .SamplerHandle = m_SamplerHandle,
            .Output = m_OutputId,
        };

        for (const Unique<ScenePass>& pass : m_Passes)
        {
            pass->Configure(m_Settings);
            pass->Resize(m_Extent);
            pass->Declare(graph, io);
        }

        m_Internal->Graph = graph.Compile();
    }

    void SceneRenderer::Resize(const uvec2 extent)
    {
        m_Extent = extent;
        CreateOutput();
        CreateGBuffer();
        CreateHdr();
        Rebuild();
    }

    void SceneRenderer::Configure(const SceneRendererSettings& settings)
    {
        m_Settings = settings;
        Rebuild();
    }

    void SceneRenderer::Execute(CommandBuffer& cmd, const SceneView& view)
    {
        // Write the current exposure into the tonemap material's Exposure param.
        // The material block is ring-buffered, so a per-frame write lands in this
        // frame's region with no stall and no frames-in-flight hazard; the tonemap
        // PostProcessScenePass reads it back through the unified material block.
        if (m_TonemapMaterial.IsLoaded())
            const_cast<Material&>(*m_TonemapMaterial.Get()).SetParam("Exposure", m_Settings.Exposure);

        // Select the scene's directional light into the per-frame view by value:
        // the first Light entity, or a zero-intensity default when the scene has
        // none. A zero-intensity light contributes nothing to the lighting pass's
        // N·L term, so a scene with no light renders flat-ambient — never pure
        // black, never asserting. The caller's view.Light, if any, is overwritten:
        // the renderer owns this selection.
        SceneView resolvedView = view;
        resolvedView.Light = Light{.Intensity = 0.0f};
        for (auto [entity, light] : const_cast<Scene&>(view.World).View<Light>())
        {
            resolvedView.Light = light;
            break; // the first Light entity wins
        }

        const RenderGraph::ImportBinding bindings[] = {
            {m_AlbedoId, m_AlbedoView},
            {m_NormalId, m_NormalView},
            {m_DepthId, m_DepthView},
            {m_HdrId, m_HdrView},
            {m_OutputId, m_OutputView},
        };
        m_Internal->Graph->Execute(cmd, bindings, &resolvedView);
    }

    Ref<ImageView> SceneRenderer::GetOutput() const { return m_OutputView; }
    Ref<ImageView> SceneRenderer::GetAlbedoView() const { return m_AlbedoView; }
    Ref<ImageView> SceneRenderer::GetNormalView() const { return m_NormalView; }
    Ref<ImageView> SceneRenderer::GetDepthView() const { return m_DepthView; }
    Ref<ImageView> SceneRenderer::GetHdrView() const { return m_HdrView; }
}
