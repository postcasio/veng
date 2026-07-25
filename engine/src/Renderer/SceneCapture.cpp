#include <Veng/Renderer/SceneCapture.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/Camera.h>

namespace Veng::Renderer
{
    namespace
    {
        // The shared fullscreen vertex stage and the plain copy fragment (the TAA history
        // copy: an unclamped passthrough — the capture samples full-extent sources, so the
        // sub-rect-aware scene_color_copy is unnecessary).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId CopyFragId{0x07F31C1EC98A29BFULL};
        // The octahedral resample fragment (atlas → octahedral map).
        constexpr AssetId OctahedralFragId{0xF79D3A3F132A0D2DULL};

        // The renderer's pre-tonemap HDR format, which the atlas and octahedral map carry
        // through unchanged so the capture output stays linear HDR.
        constexpr Format CaptureFormat = Format::RGBA16Sfloat;

        // The face copy push block, matching taa_history_copy.frag PushConstants.
        struct CopyPush
        {
            u32 SourceTexture;
            u32 Sampler;
        };

        // The resample push block, matching capture_octahedral.frag PushConstants.
        struct OctahedralPush
        {
            u32 AtlasTexture;
            u32 Sampler;
        };

        // The six face bases, +X -X +Y -Y +Z -Z. Mirrors capture_octahedral.frag.slang's
        // FaceForward/FaceUp exactly — the two must not drift.
        constexpr vec3 FaceForward[6] = {
            {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
        };
        constexpr vec3 FaceUp[6] = {
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
            {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        };
    }

    Unique<SceneCapture> SceneCapture::Create(const SceneCaptureInfo& info)
    {
        return Unique<SceneCapture>(new SceneCapture(info));
    }

    SceneCapture::SceneCapture(const SceneCaptureInfo& info)
        : m_Context(info.Context), m_FaceResolution(info.FaceResolution)
    {
        VE_ASSERT(info.FaceResolution > 0, "SceneCapture FaceResolution must be > 0");

        m_Renderer = SceneRenderer::Create({
            .Context = m_Context,
            .Assets = info.Assets,
            .OutputFormat = m_Context.GetOutputFormat(),
            .Extent = {m_FaceResolution, m_FaceResolution},
            .Settings = info.Settings,
        });

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();

        // The capture samples the renderer's pre-tonemap HDR (the lit scene the tonemap
        // would consume), so the output is linear radiance a consuming material can fold
        // into its own shading. The HDR image is created once at renderer construction and
        // the capture never resizes, so one registration holds.
        m_HdrHandle = bindless.Register(m_Renderer->GetHdrView());

        const u32 res = m_FaceResolution;
        m_AtlasImage = Image::Create(m_Context, {
                                                    .Name = "SceneCapture Atlas",
                                                    .Extent = {res * 3, res * 2, 1},
                                                    .Format = CaptureFormat,
                                                    .Usage = ImageUsage::ColorAttachment |
                                                             ImageUsage::Sampled,
                                                });
        m_AtlasView =
            ImageView::Create(m_Context, {.Name = "SceneCapture Atlas View", .Image = m_AtlasImage});
        m_AtlasHandle = bindless.Register(m_AtlasView);

        // The octahedral map at 2R×2R holds roughly the six faces' solid-angle density.
        m_OctahedralImage = Image::Create(m_Context, {
                                                         .Name = "SceneCapture Octahedral",
                                                         .Extent = {res * 2, res * 2, 1},
                                                         .Format = CaptureFormat,
                                                         .Usage = ImageUsage::ColorAttachment |
                                                                  ImageUsage::Sampled,
                                                     });
        m_OctahedralView = ImageView::Create(
            m_Context, {.Name = "SceneCapture Octahedral View", .Image = m_OctahedralImage});
        m_OctahedralHandle = bindless.Register(m_OctahedralView);

        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "SceneCapture Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Nearest,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                               });
        m_SamplerHandle = bindless.Register(m_Sampler);

        auto loadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result =
                info.Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneCapture: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };
        const AssetHandle<Veng::Shader> vs = loadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> copyFs = loadShader(CopyFragId, "copy fragment");
        const AssetHandle<Veng::Shader> octFs =
            loadShader(OctahedralFragId, "octahedral resample fragment");

        m_CopyLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneCapture Copy Layout",
                           .PushConstantRanges = {PushConstantRange::Of<CopyPush>(
                               ShaderStage::Fragment)},
                       });
        m_CopyPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "SceneCapture Copy Pipeline",
                           .ColorAttachments = {{.Format = CaptureFormat}},
                           .PipelineLayout = m_CopyLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = copyFs.Get()->Module},
                               },
                       });

        m_OctahedralLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneCapture Octahedral Layout",
                           .PushConstantRanges = {PushConstantRange::Of<OctahedralPush>(
                               ShaderStage::Fragment)},
                       });
        m_OctahedralPipeline = GraphicsPipeline::Create(
            m_Context, {
                           .Name = "SceneCapture Octahedral Pipeline",
                           .ColorAttachments = {{.Format = CaptureFormat}},
                           .PipelineLayout = m_OctahedralLayout,
                           .ShaderStages =
                               {
                                   {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                   {.Stage = ShaderStage::Fragment, .Module = octFs.Get()->Module},
                               },
                       });
    }

    SceneCapture::~SceneCapture()
    {
        if (m_DriveList != nullptr)
        {
            std::erase(*m_DriveList, this);
        }

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HdrHandle);
        bindless.Release(m_AtlasHandle);
        bindless.Release(m_OctahedralHandle);
        bindless.Release(m_SamplerHandle);
    }

    void SceneCapture::SetView(const CaptureView& view)
    {
        m_View = view;
        m_ViewFresh = true;
    }

    void SceneCapture::AttachToDriveList(vector<SceneCapture*>& driveList)
    {
        VE_ASSERT(m_DriveList == nullptr,
                  "SceneCapture is already registered to an Application's capture drive-list");
        m_DriveList = &driveList;
    }

    void SceneCapture::Render(CommandBuffer& cmd)
    {
        // Push-to-render: nothing fresh (or no world) records nothing, freezing the last map.
        if (!m_ViewFresh || m_View.World == nullptr)
        {
            return;
        }
        m_ViewFresh = false;

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 res = m_FaceResolution;

        // One face per Render, round-robin — a full refresh amortizes over six pushes. The
        // face renderer's per-frame ring buffers (DrawData, the point-field tables) hold one
        // region per frame-in-flight, so a renderer supports one Execute per frame; a second
        // same-frame Execute would overwrite the region the first's recorded draws still
        // read at submit. Amortizing also caps the capture at one small scene render per
        // frame instead of six.
        const u32 face = m_NextFace;
        m_NextFace = (m_NextFace + 1) % FaceCount;

        CameraView camera;
        camera.SetPerspective(glm::radians(90.0f), 1.0f, m_View.Near, m_View.Far);
        camera.SetView(m_View.Position, m_View.Position + FaceForward[face], FaceUp[face]);

        m_Renderer->Execute(cmd, SceneView{.World = *m_View.World,
                                           .Camera = camera,
                                           .Delta = 0.0f,
                                           .Exclude = m_View.Exclude});
        cmd.PrepareForAccess(m_Renderer->GetHdrView(), AccessKind::Sample);

        // Tile the face's HDR into its atlas cell. The atlas persists across frames (only the
        // first-ever render clears it), so the five untouched cells keep their last capture.
        cmd.PrepareForAccess(m_AtlasView, AccessKind::ColorAttachment);
        cmd.BeginRendering({
            .Extent = {res * 3, res * 2},
            .ColorAttachments = {{
                .ImageView = m_AtlasView,
                .LoadOp = m_AtlasCleared ? LoadOp::Load : LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        m_AtlasCleared = true;
        cmd.BindPipeline(m_CopyPipeline);
        const ivec2 cell{static_cast<i32>((face % 3) * res), static_cast<i32>((face / 3) * res)};
        cmd.SetViewport(cell, {res, res});
        cmd.SetScissor(cell, {res, res});
        registry.Bind(cmd);
        cmd.PushConstants(CopyPush{
            .SourceTexture = m_HdrHandle.Index,
            .Sampler = m_SamplerHandle.Index,
        });
        cmd.DrawFullscreenTriangle();
        cmd.EndRendering();

        // Resample the finished atlas into the octahedral map and leave it sampled for the
        // consuming material this frame.
        cmd.PrepareForAccess(m_AtlasView, AccessKind::Sample);
        cmd.PrepareForAccess(m_OctahedralView, AccessKind::ColorAttachment);
        cmd.BeginRendering({
            .Extent = {res * 2, res * 2},
            .ColorAttachments = {{
                .ImageView = m_OctahedralView,
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            }},
        });
        cmd.BindPipeline(m_OctahedralPipeline);
        cmd.SetViewport({0, 0}, {res * 2, res * 2});
        cmd.SetScissor({0, 0}, {res * 2, res * 2});
        registry.Bind(cmd);
        cmd.PushConstants(OctahedralPush{
            .AtlasTexture = m_AtlasHandle.Index,
            .Sampler = m_SamplerHandle.Index,
        });
        cmd.DrawFullscreenTriangle();
        cmd.EndRendering();
        cmd.PrepareForAccess(m_OctahedralView, AccessKind::Sample);
    }
}
