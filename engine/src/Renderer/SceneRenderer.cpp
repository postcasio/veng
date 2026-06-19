#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>

#include "ShadowScenePass.h"

#include <array>
#include <cmath>
#include <span>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

        // The core bloom PostProcess materials.
        constexpr AssetId BloomBrightMaterialId{0xB1C79EF4EAC3F697ULL};
        constexpr AssetId BloomBlurHMaterialId{0x7061083E93A8D7FFULL};
        constexpr AssetId BloomBlurVMaterialId{0x00CC2DEC566FFF58ULL};
        constexpr AssetId BloomCompositeMaterialId{0x19FC4F575D6CFE6CULL};

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
        // samples the g-buffer through, the current frame's view-constants region
        // index, and the light list's frame base + live count. Per-view data
        // (InvViewProj, camera position) rides the ring-buffered view-constants
        // buffer; the lights ride the ring-buffered light buffer. Push constants
        // carry only per-invocation bindless indices and the light count. Matches
        // the shader's PushConstants byte-for-byte (eight packed u32s).
        struct LightingPushConstants
        {
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 LightBase;
            u32 LightCount;
        };

        // One light packed for the ring-buffered light buffer (set-0 binding 6),
        // std430-friendly, matching the shader's GpuLight byte-for-byte. Position is
        // the entity's world transform; Type is a LightType cast to float.
        struct GpuLight
        {
            vec4 PositionRange;   // xyz world position, w range
            vec4 DirectionType;   // xyz travel direction, w LightType
            vec4 ColorIntensity;  // rgb linear color, a intensity
            vec4 Cone;            // x cos(inner), y cos(outer), zw pad
        };

        static_assert(sizeof(GpuLight) == BindlessRegistry::LightStride,
                      "GpuLight must match the bindless light buffer stride");

        // The per-frame view-constants block the lighting pass reads from set-0
        // binding 5. Mirrors the core material.slang ViewConstants byte-for-byte.
        struct ViewConstantsBlock
        {
            mat4 InvViewProj;
            vec4 CameraPosition; // xyz; w unused
            mat4 LightViewProj;  // directional light's world → light-clip transform
            vec4 ShadowParams;   // x shadow-map handle, y sampler handle, z 1/resolution, w enabled (0/1)
        };

        static_assert(sizeof(ViewConstantsBlock) <= BindlessRegistry::ViewConstantsStride,
                      "ViewConstantsBlock must fit one ring-buffered view-constants region");

        // The directional shadow camera's fixed orthographic box, centered on the
        // world origin. No scene-bounds/AABB fit exists yet, so the box is sized
        // generously to cover the sample scene's casters and receivers; a tight fit
        // (and cascades) is the quality follow-on this machinery extends.
        constexpr f32 ShadowOrthoHalfExtent = 6.0f;
        constexpr f32 ShadowOrthoDepth = 20.0f;

        // The directional light's world → light-clip transform: an orthographic
        // projection looking along the light's travel direction toward the origin
        // from a point pulled back along -direction. Y is flipped for Vulkan clip
        // space, matching Camera's projection convention, and the depth range is
        // mapped to [0, 1] (GLM's _ZO ortho) so the sampled depth compares directly
        // against the shadow map's stored depth.
        mat4 DirectionalLightViewProj(vec3 travelDirection)
        {
            const vec3 dir = glm::normalize(travelDirection);

            // A stable up vector: the world up unless the light points (nearly)
            // straight up or down, in which case fall back to +Z.
            const vec3 worldUp = std::abs(dir.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);

            const vec3 eye = -dir * (ShadowOrthoDepth * 0.5f);
            const mat4 lightView = glm::lookAt(eye, vec3(0.0f), worldUp);

            mat4 lightProj = glm::orthoZO(
                -ShadowOrthoHalfExtent, ShadowOrthoHalfExtent,
                -ShadowOrthoHalfExtent, ShadowOrthoHalfExtent,
                0.0f, ShadowOrthoDepth);
            lightProj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

            return lightProj * lightView;
        }

        // The shared debug-blit fragment push block: the bindless slots a fullscreen
        // blit samples one g-buffer channel through. The albedo, normal, and depth
        // visualizers all declare exactly these two fields.
        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };

        // The deferred g-buffer geometry pass: draws every (Transform, MeshRenderer)
        // entity into the renderer's imported g-buffer (G0 albedo, G1 world-normal,
        // G2 packed ORM) with the shared depth attachment. Each material's pipeline
        // writes all three color targets through its GBufferOutput. Sourced from the
        // per-frame SceneView.
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
                    .Color({
                        .Resource = io.GBufferOrm,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        // Default occlusion 1 (unoccluded), roughness/metallic/emissive 0
                        // for any background texel; a material overwrites all four.
                        .Clear = ClearColor{1.0f, 0.0f, 0.0f, 0.0f},
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
        // G1 world-normal, G2 ORM, depth) through their bindless handles, evaluates
        // a Cook-Torrance BRDF over the directional light read from the per-frame
        // view-constants buffer (reconstructing world position from depth), and
        // writes the renderer's imported HDR target. Declaring .Sample on each
        // g-buffer id lets the graph derive the attachment → shader-read transitions
        // — including the depth attachment → shader-read transition, the only depth
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
                const TextureHandle ormHandle = io.OrmHandle;
                const TextureHandle depthHandle = io.DepthHandle;
                const SamplerHandle samplerHandle = io.SamplerHandle;

                RenderGraph::PassBuilder builder = graph.AddPass("Deferred Lighting");
                builder.Color({
                        .Resource = io.Hdr,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                    })
                    .Sample(io.GBufferAlbedo)
                    .Sample(io.GBufferNormal)
                    .Sample(io.GBufferOrm)
                    .Sample(io.GBufferDepth);

                // Declaring the shadow map sampled (when the shadow pass is wired)
                // lets the graph derive its depth-attachment → shader-read barrier.
                // The lighting shader reads the shadow handle from the view-constants
                // buffer (ShadowParams), so no push field is needed.
                if (io.ShadowMap.IsValid())
                    builder.Sample(io.ShadowMap);

                builder.Execute([this, albedoHandle, normalHandle, ormHandle, depthHandle, samplerHandle](PassContext& inner)
                    {
                        const ScenePassContext ctx = Wrap(inner);
                        CommandBuffer& cmd = ctx.Cmd();
                        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                        cmd.BindPipeline(m_Pipeline);
                        cmd.SetViewport({0, 0}, m_Extent);
                        cmd.SetScissor({0, 0}, m_Extent);
                        registry.Bind(cmd);
                        cmd.PushConstants(LightingPushConstants{
                            .AlbedoTexture = albedoHandle.Index,
                            .NormalTexture = normalHandle.Index,
                            .OrmTexture = ormHandle.Index,
                            .DepthTexture = depthHandle.Index,
                            .Sampler = samplerHandle.Index,
                            .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                            .LightBase = registry.GetCurrentLightBase(),
                            .LightCount = ctx.View().LightCount,
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
        const PostProcessExtraInput extra = m_Extra;
        const bool hasExtra = extra.Texture.IsValid();

        // A pass with a second runtime-bound source declares .Sample on both ids so
        // the graph derives each one's attachment → shader-read barrier; the
        // single-source common case (tonemap, bright-pass, the blur stages) samples
        // only the primary input.
        RenderGraph::PassBuilder builder = graph.AddPass("PostProcess");
        builder.Color({
                .Resource = m_Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(input.Source);
        if (hasExtra)
            builder.Sample(extra.Source);

        builder.Execute([this, input, extra, hasExtra](PassContext& inner)
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
                if (hasExtra)
                {
                    material.SetTextureHandle(extra.TextureField, extra.Texture);
                    material.SetSamplerHandle(extra.SamplerField, extra.Sampler);
                }

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
        CreateBloom();
        Rebuild();
    }

    SceneRenderer::~SceneRenderer()
    {
        // Release the bindless slots through the per-frame retire window before the
        // images they name retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_HdrHandle);
        bindless.Release(m_BloomBrightHandle);
        bindless.Release(m_BloomBlurHHandle);
        bindless.Release(m_BloomBlurVHandle);
        bindless.Release(m_BloomResultHandle);
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

        // The bloom chain's four PostProcess materials (bright-pass, two-axis blur,
        // composite), loaded resident so each stage's PostProcessScenePass builds its
        // fullscreen pipeline against the HDR format.
        auto LoadMaterial = [this](const AssetId id, const char* what) -> AssetHandle<Material>
        {
            const AssetResult<AssetHandle<Material>> result = m_Assets.LoadSync<Material>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} material load failed: {}", what, result.error().Detail);
            return *result;
        };
        m_BloomBrightMaterial = LoadMaterial(BloomBrightMaterialId, "bloom bright-pass");
        m_BloomBlurHMaterial = LoadMaterial(BloomBlurHMaterialId, "bloom blur horizontal");
        m_BloomBlurVMaterial = LoadMaterial(BloomBlurVMaterialId, "bloom blur vertical");
        m_BloomCompositeMaterial = LoadMaterial(BloomCompositeMaterialId, "bloom composite");

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
        bindless.Release(m_OrmHandle);
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

        m_OrmImage = Image::Create(m_Context, {
            .Name = "SceneRenderer GBuffer ORM",
            .Extent = {m_Extent.x, m_Extent.y, 1},
            .Format = GBuffer::ORMFormat,
            .Usage = GBuffer::ColorUsage,
        });
        m_OrmView = ImageView::Create(m_Context, {.Name = "SceneRenderer GBuffer ORM View", .Image = m_OrmImage});

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
        m_OrmHandle = bindless.Register(m_OrmView);
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

    void SceneRenderer::CreateBloom()
    {
        // The bloom intermediates are renderer-owned and imported: each bloom stage
        // writes one and the next stage samples it through bindless. Same HDR format
        // (linear, unbounded) since bloom operates in linear HDR before tonemap, and
        // the same single-copy contract. Dropping the old Refs retires them; releasing
        // the old slots defers through the per-frame window.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_BloomBrightHandle);
        bindless.Release(m_BloomBlurHHandle);
        bindless.Release(m_BloomBlurVHandle);
        bindless.Release(m_BloomResultHandle);

        auto MakeTarget = [this](const char* name) -> std::pair<Ref<Image>, Ref<ImageView>>
        {
            Ref<Image> image = Image::Create(m_Context, {
                .Name = name,
                .Extent = {m_Extent.x, m_Extent.y, 1},
                .Format = HdrFormat,
                .Usage = HdrUsage,
            });
            Ref<ImageView> view = ImageView::Create(m_Context, {.Name = name, .Image = image});
            return {std::move(image), std::move(view)};
        };

        std::tie(m_BloomBrightImage, m_BloomBrightView) = MakeTarget("SceneRenderer Bloom Bright");
        std::tie(m_BloomBlurHImage, m_BloomBlurHView) = MakeTarget("SceneRenderer Bloom Blur H");
        std::tie(m_BloomBlurVImage, m_BloomBlurVView) = MakeTarget("SceneRenderer Bloom Blur V");
        std::tie(m_BloomResultImage, m_BloomResultView) = MakeTarget("SceneRenderer Bloom Result");

        m_BloomBrightHandle = bindless.Register(m_BloomBrightView);
        m_BloomBlurHHandle = bindless.Register(m_BloomBlurHView);
        m_BloomBlurVHandle = bindless.Register(m_BloomBlurVView);
        m_BloomResultHandle = bindless.Register(m_BloomResultView);
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
        // The bloom chain runs only in the Final mode (the debug views terminate
        // after the g-buffer); when active it inserts four PostProcess stages between
        // lighting and tonemap, so its imports are declared and bound only then.
        const bool bloomActive = m_Settings.Mode == DebugView::Final && m_Settings.Bloom;
        m_BloomActive = bloomActive;

        // The shadow pass runs only in the Final mode (the debug views terminate
        // after the g-buffer); when active it contributes a depth-only pass ahead of
        // the lighting pass and its import is declared and bound only then.
        const bool shadowActive = m_Settings.Mode == DebugView::Final && m_Settings.Shadows;
        m_ShadowActive = shadowActive;
        m_ShadowPass = nullptr;

        RenderGraph graph(m_Context);
        const ResourceId albedoId = graph.Import("SceneRenderer GBuffer Albedo");
        const ResourceId normalId = graph.Import("SceneRenderer GBuffer Normal");
        const ResourceId ormId = graph.Import("SceneRenderer GBuffer ORM");
        const ResourceId depthId = graph.Import("SceneRenderer GBuffer Depth");
        const ResourceId hdrId = graph.Import("SceneRenderer HDR");
        m_OutputId = graph.Import("SceneRenderer Output");

        m_AlbedoId = albedoId;
        m_NormalId = normalId;
        m_OrmId = ormId;
        m_DepthId = depthId;
        m_HdrId = hdrId;

        ResourceId shadowId{};
        if (shadowActive)
            shadowId = graph.Import("SceneRenderer ShadowMap");
        m_ShadowId = shadowId;

        if (bloomActive)
        {
            m_BloomBrightId = graph.Import("SceneRenderer Bloom Bright");
            m_BloomBlurHId = graph.Import("SceneRenderer Bloom Blur H");
            m_BloomBlurVId = graph.Import("SceneRenderer Bloom Blur V");
            m_BloomResultId = graph.Import("SceneRenderer Bloom Result");
        }

        m_Passes.clear();

        // The shadow pass is first when active — it writes the shadow map the
        // lighting pass samples, so the graph orders the depth write before the
        // lighting read. It survives across rebuilds at the current resolution
        // (recreated only when ShadowResolution changes), so it is rebuilt here too.
        TextureHandle shadowHandle{};
        if (shadowActive)
        {
            auto shadowPass = CreateUnique<ShadowScenePass>(m_Context, m_Assets, m_Settings.ShadowResolution);
            m_ShadowPass = shadowPass.get();
            shadowHandle = shadowPass->GetShadowHandle();
            m_Passes.push_back(std::move(shadowPass));
        }

        m_Passes.push_back(CreateUnique<GBufferScenePass>(m_Context, m_Extent));
        switch (m_Settings.Mode)
        {
            case DebugView::Final:
            {
                m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(m_Context, m_LightingPipeline, m_Extent));

                // The tonemap stage's input: the bloom composite result when bloom is
                // on, the raw HDR target otherwise. The four bloom stages sit between
                // lighting and tonemap, each sampling the prior stage's output.
                ResourceId tonemapSourceId = m_HdrId;
                TextureHandle tonemapSourceHandle = m_HdrHandle;

                if (bloomActive)
                {
                    // Bright-pass: HDR → Bright (luminance above Threshold).
                    m_Passes.push_back(CreateUnique<PostProcessScenePass>(
                        m_Context, m_BloomBrightMaterial,
                        PostProcessInput{
                            .Source = m_HdrId,
                            .SourceTexture = m_HdrHandle,
                            .Sampler = m_SamplerHandle,
                            .TextureField = "Hdr",
                            .SamplerField = "HdrSampler",
                        },
                        m_BloomBrightId, HdrFormat, m_Extent));

                    // Blur horizontal: Bright → BlurH.
                    m_Passes.push_back(CreateUnique<PostProcessScenePass>(
                        m_Context, m_BloomBlurHMaterial,
                        PostProcessInput{
                            .Source = m_BloomBrightId,
                            .SourceTexture = m_BloomBrightHandle,
                            .Sampler = m_SamplerHandle,
                            .TextureField = "Source",
                            .SamplerField = "SourceSampler",
                        },
                        m_BloomBlurHId, HdrFormat, m_Extent));

                    // Blur vertical: BlurH → BlurV (the finished single-level blur).
                    m_Passes.push_back(CreateUnique<PostProcessScenePass>(
                        m_Context, m_BloomBlurVMaterial,
                        PostProcessInput{
                            .Source = m_BloomBlurHId,
                            .SourceTexture = m_BloomBlurHHandle,
                            .Sampler = m_SamplerHandle,
                            .TextureField = "Source",
                            .SamplerField = "SourceSampler",
                        },
                        m_BloomBlurVId, HdrFormat, m_Extent));

                    // Composite: HDR + BlurV*Intensity → Result, sampling two inputs.
                    auto composite = CreateUnique<PostProcessScenePass>(
                        m_Context, m_BloomCompositeMaterial,
                        PostProcessInput{
                            .Source = m_HdrId,
                            .SourceTexture = m_HdrHandle,
                            .Sampler = m_SamplerHandle,
                            .TextureField = "Hdr",
                            .SamplerField = "HdrSampler",
                        },
                        m_BloomResultId, HdrFormat, m_Extent);
                    composite->SetExtraInput(PostProcessExtraInput{
                        .Source = m_BloomBlurVId,
                        .Texture = m_BloomBlurVHandle,
                        .Sampler = m_SamplerHandle,
                        .TextureField = "Bloom",
                        .SamplerField = "BloomSampler",
                    });
                    m_Passes.push_back(std::move(composite));

                    tonemapSourceId = m_BloomResultId;
                    tonemapSourceHandle = m_BloomResultHandle;
                }

                m_Passes.push_back(CreateUnique<PostProcessScenePass>(
                    m_Context, m_TonemapMaterial,
                    PostProcessInput{
                        .Source = tonemapSourceId,
                        .SourceTexture = tonemapSourceHandle,
                        .Sampler = m_SamplerHandle,
                        .TextureField = "Hdr",
                        .SamplerField = "HdrSampler",
                    },
                    m_OutputId, m_OutputFormat, m_Extent));
                break;
            }
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
            .GBufferOrm = ormId,
            .GBufferDepth = depthId,
            .AlbedoHandle = m_AlbedoHandle,
            .NormalHandle = m_NormalHandle,
            .OrmHandle = m_OrmHandle,
            .DepthHandle = m_DepthHandle,
            .Hdr = hdrId,
            .HdrHandle = m_HdrHandle,
            .SamplerHandle = m_SamplerHandle,
            .ShadowMap = shadowId,
            .ShadowHandle = shadowHandle,
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
        CreateBloom();
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

        // The bloom Threshold/Intensity are per-frame view values (the same
        // ring-buffered, stall-free write path as Exposure) — tuning them never
        // recompiles. Written only when the chain is active.
        if (m_BloomActive)
        {
            if (m_BloomBrightMaterial.IsLoaded())
                const_cast<Material&>(*m_BloomBrightMaterial.Get()).SetParam("Threshold", view.BloomThreshold);
            if (m_BloomCompositeMaterial.IsLoaded())
                const_cast<Material&>(*m_BloomCompositeMaterial.Get()).SetParam("Intensity", view.BloomIntensity);
        }

        // Walk the scene's (Transform, Light) entities, capped at MaxLights, packing
        // each into a GpuLight: its world position from the entity transform, its
        // type/direction/color/intensity, and the spot cone cosines. A scene with no
        // Light packs zero lights, so the lighting pass loops zero times and the
        // result is flat-ambient — never pure black, never asserting. The caller's
        // view.LightCount, if any, is overwritten: the renderer owns this selection.
        std::array<GpuLight, SceneView::MaxLights> packedLights{};
        u32 lightCount = 0;
        bool haveDirectional = false;
        vec3 directionalTravel{0.0f, -1.0f, 0.0f};
        for (auto [entity, light] : const_cast<Scene&>(view.World).View<Light>())
        {
            if (lightCount >= SceneView::MaxLights)
                break;

            // The shadow map shadows the first directional light; its travel
            // direction drives the light-space matrix the shadow and lighting passes
            // both use.
            if (!haveDirectional && light.Type == LightType::Directional)
            {
                haveDirectional = true;
                directionalTravel = light.Direction;
            }

            const mat4 world = WorldMatrix(view.World, entity);
            const vec3 worldPos = vec3(world[3]);
            // The cone bounds are stored as cosines so the shader compares against
            // dot products directly; cos is monotonically decreasing in angle, so
            // cos(inner) >= cos(outer).
            const f32 cosInner = std::cos(light.InnerCone);
            const f32 cosOuter = std::cos(light.OuterCone);

            packedLights[lightCount] = GpuLight{
                .PositionRange = vec4(worldPos, light.Range),
                .DirectionType = vec4(light.Direction, static_cast<f32>(light.Type)),
                .ColorIntensity = vec4(light.Color, light.Intensity),
                .Cone = vec4(cosInner, cosOuter, 0.0f, 0.0f),
            };
            ++lightCount;
        }

        // The directional light's light-space matrix, recomputed each frame from the
        // first directional light. The shadow pass reads it back as the light-space
        // MVP and the lighting pass projects fragments into shadow space with it.
        const mat4 lightViewProj = DirectionalLightViewProj(directionalTravel);

        SceneView resolvedView = view;
        resolvedView.LightCount = lightCount;
        resolvedView.LightViewProj = lightViewProj;

        BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        registry.WriteLights(std::as_bytes(std::span(packedLights.data(), lightCount)));

        // ShadowParams gate the lighting pass's shadow path: the shadow-map and
        // sampler bindless indices, the texel size (1/resolution) for the PCF kernel,
        // and an enabled flag. Enabled only when the shadow pass is wired AND a
        // directional light exists this frame — otherwise the lighting pass reads
        // full visibility and never samples the (possibly stale) slot.
        vec4 shadowParams{0.0f, 0.0f, 0.0f, 0.0f};
        if (m_ShadowActive && m_ShadowPass && haveDirectional)
        {
            shadowParams = vec4(
                static_cast<f32>(m_ShadowPass->GetShadowHandle().Index),
                static_cast<f32>(m_SamplerHandle.Index),
                1.0f / static_cast<f32>(m_ShadowPass->GetResolution()),
                1.0f);
        }

        // Pack the per-frame view constants — the inverse view-projection (for
        // world-position reconstruction), the camera position, the directional
        // light's light-space matrix, and the shadow params — and write them into
        // the current frame's ring-buffered region. The lighting pass reads them
        // back through set-0 binding 5.
        const mat4 viewProj = view.Camera.ViewProjection();
        const ViewConstantsBlock viewConstants{
            .InvViewProj = glm::inverse(viewProj),
            .CameraPosition = vec4(view.Camera.GetPosition(), 0.0f),
            .LightViewProj = lightViewProj,
            .ShadowParams = shadowParams,
        };
        registry.WriteViewConstants(std::as_bytes(std::span(&viewConstants, 1)));

        // Every declared import must be bound; the bloom imports are declared only
        // when the chain is active, so they are appended only then.
        vector<RenderGraph::ImportBinding> bindings = {
            {m_AlbedoId, m_AlbedoView},
            {m_NormalId, m_NormalView},
            {m_OrmId, m_OrmView},
            {m_DepthId, m_DepthView},
            {m_HdrId, m_HdrView},
            {m_OutputId, m_OutputView},
        };
        if (m_ShadowActive && m_ShadowPass)
            bindings.push_back({m_ShadowId, m_ShadowPass->GetShadowView()});
        if (m_BloomActive)
        {
            bindings.push_back({m_BloomBrightId, m_BloomBrightView});
            bindings.push_back({m_BloomBlurHId, m_BloomBlurHView});
            bindings.push_back({m_BloomBlurVId, m_BloomBlurVView});
            bindings.push_back({m_BloomResultId, m_BloomResultView});
        }
        m_Internal->Graph->Execute(cmd, bindings, &resolvedView);
    }

    Ref<ImageView> SceneRenderer::GetOutput() const { return m_OutputView; }
    Ref<ImageView> SceneRenderer::GetAlbedoView() const { return m_AlbedoView; }
    Ref<ImageView> SceneRenderer::GetNormalView() const { return m_NormalView; }
    Ref<ImageView> SceneRenderer::GetOrmView() const { return m_OrmView; }
    Ref<ImageView> SceneRenderer::GetDepthView() const { return m_DepthView; }
    Ref<ImageView> SceneRenderer::GetHdrView() const { return m_HdrView; }
    Ref<ImageView> SceneRenderer::GetBloomResultView() const { return m_BloomResultView; }
}
