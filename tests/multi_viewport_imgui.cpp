// Multi-viewport ImGui interaction test: two ImGui windows side by side, each
// hosting a Veng::Renderer::Viewport that renders a distinctly-colored scene
// (a white plane squarely facing the camera, lit by a single colored directional
// light — red on the left, green on the right). This is the editor's real shape:
// each SceneViewportPanel owns an Offscreen viewport and draws its output with
// ImGui::Image; two open panels put two viewports on screen in one frame.
//
// The regression it guards is the multi-viewport rendering bug: when two lit
// viewports render in the same frame, only the last one registered comes out
// correct — every earlier viewport's deferred-lighting result is lost (it reads
// back black), so the two side-by-side panels do not both show their own scene.
// (The existing Albedo-mode multi_viewport_isolation case does not catch this; the
// fault is specific to the Final/lighting path, which this exercises.) The test
// drives the full ImGui interaction (BeginFrame, build both windows,
// ImGuiLayer::Render into the UI's offscreen image), reads back that composited
// image, and asserts the left half is red-dominant and the right half is
// green-dominant — distinct colors the bug (a black, unlit earlier viewport) fails.
//
// The UI image is ColorAttachment|Sampled (not TransferSrc), so it cannot be
// downloaded directly; a fullscreen pass samples it into a TransferSrc target the
// test owns (the core composite shader with the same handle for both sources is a
// passthrough of the UI color), standing in for the swapchain composite that a
// headless run has no surface for.
//
// Skips cleanly (exit 77) on a machine with no usable Vulkan ICD.

#include <cstdio>
#include <filesystem>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Time.h>
#include <Veng/Vendor/ImGui.h>
#include <Veng/Window.h>

#include <support/GpuProbe.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The white-plane fixture's default material instance (decimal 9000003). The pack
    // maps texture id 9001 to a solid-white image, so the brick material renders a
    // white plane whose only color comes from the scene's light.
    constexpr AssetId WhitePlaneInstanceId{0x895443ULL};

    // Core-pack shader ids: the fullscreen-triangle vertex stage and the composite
    // fragment stage, used to sample the UI image into a downloadable target.
    constexpr AssetId FullscreenVertId{17612966569144354344ULL};
    constexpr AssetId CompositeFragId{5462853072295605294ULL};

    int g_Failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++g_Failures;
        }
    }

    // One RGBA16F texel decoded to a linear vec3 (the UI image and the readback
    // target are both RGBA16Sfloat).
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // The core composite fragment's push-constant block. Passing the same texture
    // handle for both sources makes lerp(scene, ui, ui.a) a passthrough of the
    // sampled color; EncodeMode 0 is the linear passthrough transfer.
    struct CompositePush
    {
        u32 SceneTexture;
        u32 ImGuiTexture;
        u32 Sampler;
        u32 EncodeMode;
        f32 PaperWhiteNits;
        f32 PeakNits;
    };

    // Samples `source` through a fullscreen pass into a fresh RGBA16Sfloat
    // TransferSrc target and returns the downloaded pixels — the stand-in for the
    // swapchain composite, which needs a surface a headless run does not have.
    vector<u8> SampleToOffscreen(Context& context, AssetManager& assets,
                                 const Ref<ImageView>& source, uvec2 extent)
    {
        const AssetResult<AssetHandle<Shader>> vs = assets.LoadSync<Shader>(FullscreenVertId);
        const AssetResult<AssetHandle<Shader>> fs = assets.LoadSync<Shader>(CompositeFragId);
        Check(vs.has_value(), "load fullscreen vertex shader");
        Check(fs.has_value(), "load composite fragment shader");

        const Ref<PipelineLayout> layout = PipelineLayout::Create(
            context, {
                         .Name = "MultiViewport Readback Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<CompositePush>(ShaderStage::Fragment),
                             },
                     });

        const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            context, {
                         .Name = "MultiViewport Readback Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA16Sfloat}},
                         .PipelineLayout = layout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vs->Get()->Module},
                                 {.Stage = ShaderStage::Fragment, .Module = fs->Get()->Module},
                             },
                     });

        const Ref<Sampler> sampler =
            Sampler::Create(context, {
                                         .Name = "MultiViewport Readback Sampler",
                                         .MagFilter = Filter::Nearest,
                                         .MinFilter = Filter::Nearest,
                                         .AddressModeU = AddressMode::ClampToEdge,
                                         .AddressModeV = AddressMode::ClampToEdge,
                                         .AddressModeW = AddressMode::ClampToEdge,
                                     });

        const Ref<Image> target = Image::Create(
            context, {
                         .Name = "MultiViewport Readback Output",
                         .Extent = {extent.x, extent.y, 1},
                         .Format = Format::RGBA16Sfloat,
                         .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                     });
        const Ref<ImageView> targetView = ImageView::Create(
            context, {.Name = "MultiViewport Readback Output View", .Image = target});

        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const TextureHandle textureHandle = bindless.Register(source);
        const SamplerHandle samplerHandle = bindless.Register(sampler);

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId sourceId = graph.Import("MultiViewport UI Source");
                const ResourceId outputId = graph.Import("MultiViewport Readback Target");

                graph.AddPass("sample ui image")
                    .Color({
                        .Resource = outputId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(sourceId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& passCmd = ctx.Cmd();
                            passCmd.BindPipeline(pipeline);
                            passCmd.SetViewport({0, 0}, extent);
                            passCmd.SetScissor({0, 0}, extent);
                            bindless.Bind(passCmd);
                            passCmd.PushConstants(CompositePush{
                                .SceneTexture = textureHandle.Index,
                                .ImGuiTexture = textureHandle.Index,
                                .Sampler = samplerHandle.Index,
                                .EncodeMode = 0,
                                .PaperWhiteNits = 0.0f,
                                .PeakNits = 0.0f,
                            });
                            passCmd.DrawFullscreenTriangle();
                        });

                const RenderGraph::ImportBinding bindings[] = {
                    {.Id = sourceId, .View = source},
                    {.Id = outputId, .View = targetView},
                };
                graph.Compile()->Execute(cmd, bindings);
            });

        vector<u8> pixels = target->Download();
        bindless.Release(textureHandle);
        bindless.Release(samplerHandle);
        return pixels;
    }

    // A white plane squarely facing an overhead camera, lit by a single directional
    // light traveling straight down onto the plane's +Y face (N·L = 1, fully lit).
    // The returned scene owns nothing GPU-side beyond the shared plane mesh handle.
    Unique<Scene> BuildLitPlaneScene(TypeRegistry& types, const AssetHandle<Mesh>& plane,
                                     vec3 lightColor)
    {
        Unique<Scene> scene = Scene::Create(types);

        const Entity planeEntity = scene->CreateEntity();
        scene->Add<Transform>(planeEntity);
        scene->Add<MeshRenderer>(planeEntity).Mesh = plane;

        const Entity lightEntity = scene->CreateEntity();
        scene->Add<Light>(lightEntity) = Light{
            .Type = LightType::Directional,
            .Direction = vec3(0.0f, -1.0f, 0.0f), // travels down onto the +Y plane face
            .Color = lightColor,
            .Intensity = 2.5f,
        };

        return scene;
    }

    // An overhead camera looking straight down at the plane's center; the up axis is
    // -Z because the view direction is parallel to the world Y axis.
    CameraView OverheadCamera(uvec2 extent)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f),
                              static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f,
                              100.0f);
        camera.SetView(vec3(0.0f, 3.0f, 0.0f), vec3(0.0f), vec3(0.0f, 0.0f, -1.0f));
        return camera;
    }
}

int main()
{
    if (!Test::HasVulkanDriver())
    {
        return 77;
    }

    Time::Initialize();

    // A windowed context: the ImGui Vulkan backend needs a swapchain image count,
    // which is only available with a window. The window is never shown to a frame
    // loop; the UI is rendered into the layer's offscreen image and read back.
    const Unique<Window> window = Window::Create({
        .Extent = {640, 480},
        .Resizable = false,
        .Title = "Multi-Viewport ImGui Test",
        .CaptureMouse = false,
    });

    Context context;
    context.Initialize(
        {
            .ApplicationName = "Multi-Viewport ImGui Test",
            .HeadlessExtent = {640, 480},
        },
        window.get());

    Unique<TaskSystem> tasks = CreateUnique<TaskSystem>(TaskSystemInfo{.WorkerCount = 2});
    Unique<ImGuiLayer> imgui = ImGuiLayer::Create({}, context, *window);

    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);

        AssetManager assets(context, *tasks, types);

        // Cook the white-plane fixture pack in-process (over the auto-mounted core
        // pack, which supplies the canonical layout and the lighting/composite shaders).
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_multi_viewport_imgui.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "white_plane_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        Check(cookResult.has_value(), "cook white-plane fixture pack");

        const VoidResult mountResult = assets.Mount(outArchive);
        Check(mountResult.has_value(), "mount white-plane fixture pack");

        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(WhitePlaneInstanceId);
        Check(material.has_value() && material->IsLoaded(), "load white-plane material");

        // A large white plane; both scenes light the one shared mesh differently.
        const Ref<Mesh> plane = Mesh::BuildSync(
            context,
            Primitives::Plane(vec2(10.0f), uvec2(1),
                              material.has_value() ? *material : AssetHandle<MaterialInstance>{}),
            "White Plane");
        // Adopt once; both scenes reference the same resident mesh handle.
        const AssetHandle<Mesh> planeHandle = assets.Adopt(plane);

        const Unique<Scene> redScene =
            BuildLitPlaneScene(types, planeHandle, vec3(1.0f, 0.05f, 0.05f));
        const Unique<Scene> greenScene =
            BuildLitPlaneScene(types, planeHandle, vec3(0.05f, 1.0f, 0.05f));

        // Two Offscreen viewports, one per scene — the editor's per-panel viewport
        // shape. Each renders its own scene through its own overhead camera.
        constexpr uvec2 viewportExtent{300, 460};

        const Unique<Viewport> redView = Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = viewportExtent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
            .Role = ViewportRole::Offscreen,
        });
        redView->SetViewState(
            {.World = redScene.get(), .Camera = OverheadCamera(viewportExtent), .Delta = 0.0f});

        const Unique<Viewport> greenView = Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = viewportExtent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
            .Role = ViewportRole::Offscreen,
        });
        greenView->SetViewState(
            {.World = greenScene.get(), .Camera = OverheadCamera(viewportExtent), .Delta = 0.0f});

        vector<Viewport*> driveList;
        driveList.emplace_back(redView.get());
        redView->AttachToDriveList(driveList);
        driveList.emplace_back(greenView.get());
        greenView->AttachToDriveList(driveList);

        // One sampler both UI textures are drawn through; each viewport's output is
        // registered with the ImGui backend as a drawable texture (as a panel does).
        const Ref<Sampler> uiSampler = Sampler::Create(context, {
                                                                    .Name = "UI Viewport Sampler",
                                                                    .MagFilter = Filter::Linear,
                                                                    .MinFilter = Filter::Linear,
                                                                });
        const Ref<ImGuiTexture> redTexture =
            imgui->CreateTexture(*uiSampler, *redView->GetOutput());
        const Ref<ImGuiTexture> greenTexture =
            imgui->CreateTexture(*uiSampler, *greenView->GetOutput());

        // Build the ImGui frame: two borderless, padless windows tiling the display
        // left/right, each filled by its viewport's image. Splitting by the ImGui
        // display size (not the pixel extent) keeps the layout DPI-independent.
        imgui->BeginFrame();
        {
            const ImGuiIO& io = ImGui::GetIO();
            const float halfWidth = io.DisplaySize.x * 0.5f;
            const float fullHeight = io.DisplaySize.y;
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(halfWidth, fullHeight));
            ImGui::Begin("Viewport Red", nullptr, flags);
            ImGui::Image(static_cast<ImTextureID>(redTexture->GetTextureId()),
                         ImGui::GetContentRegionAvail());
            ImGui::End();

            ImGui::SetNextWindowPos(ImVec2(halfWidth, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(halfWidth, fullHeight));
            ImGui::Begin("Viewport Green", nullptr, flags);
            ImGui::Image(static_cast<ImTextureID>(greenTexture->GetTextureId()),
                         ImGui::GetContentRegionAvail());
            ImGui::End();

            ImGui::PopStyleVar(2);
        }

        // One command stream: render both viewports (drive-list order, each leaving
        // its output sampleable), then render the UI that samples them.
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                for (Viewport* viewport : driveList)
                {
                    viewport->Render(cmd);
                }
                imgui->Render(cmd);
            });

        // Read the composited UI image back through the fullscreen sample pass.
        const uvec2 uiExtent = context.GetRenderExtent();
        const Ref<ImageView> uiView = ImageView::Create(
            context, {.Name = "UI Output Readback View", .Image = imgui->GetOutputImage()});
        const vector<u8> pixels = SampleToOffscreen(context, assets, uiView, uiExtent);
        Check(pixels.size() == static_cast<size_t>(uiExtent.x) * uiExtent.y * 8,
              "readback pixel buffer is RGBA16F of the UI extent");

        // Sample the center of each half (quarter and three-quarter width) — clear of
        // the seam between the two windows.
        const vec3 left = DecodeTexel(pixels, uiExtent.x, uiExtent.x / 4, uiExtent.y / 2);
        const vec3 right = DecodeTexel(pixels, uiExtent.x, uiExtent.x * 3 / 4, uiExtent.y / 2);

        std::fprintf(stderr, "[info] left=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f)\n", left.r,
                     left.g, left.b, right.r, right.g, right.b);

        // Each half carries its own viewport's light color.
        Check(left.r > 0.1f && left.r > left.g && left.r > left.b,
              "left half is red-dominant (its viewport's red light)");
        Check(right.g > 0.1f && right.g > right.r && right.g > right.b,
              "right half is green-dominant (its viewport's green light)");

        // The two halves are distinctly different — the direct guard against the
        // multi-viewport bug, where an earlier viewport renders black (or otherwise
        // fails to carry its own scene) while only the last one is correct.
        Check(left.r > right.r + 0.1f, "left half is markedly redder than the right");
        Check(right.g > left.g + 0.1f, "right half is markedly greener than the left");

        std::filesystem::remove(outArchive);
    }

    // Teardown mirrors Application::Dispose: drain in-flight work, then release the
    // ImGui layer and task system before the context disposes its resources.
    context.WaitIdle();
    imgui.reset();
    tasks.reset();
    context.DisposeResources();
    context.Dispose();

    return g_Failures == 0 ? 0 : 1;
}
