// MaterialPreview GPU test: brings up a windowed Context + ImGuiLayer (the
// preview owns a Ref<ImGuiTexture>, which needs the ImGui Vulkan backend), cooks
// the brick g-buffer fixture in-process, and exercises the preview surface.
//
// It proves the preview renders the brick material on a sphere into a 256² target
// exposed as a non-null ImGuiTexture, and that SetMaterial swaps cleanly. The
// preview owns an Offscreen Viewport; the test drives it directly (Update pushes
// the view, the viewport's Render records the scene), standing in for the engine
// drive-list. A second viewport-style SceneRenderer is recorded alongside it in
// one command stream, so the two-renderer cross-graph handoff (each renderer
// brackets its own output's Sample ↔ ColorAttachment over disjoint targets) is
// exercised under the validation gate — not just an isolated preview.
//
// Skips cleanly (exit 77) on a machine with no usable Vulkan ICD via
// Test::HasVulkanDriver().

#include <cstdio>
#include <filesystem>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Time.h>
#include <Veng/Window.h>

#include <support/GpuProbe.h>

#include "material/MaterialPreview.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The brick g-buffer fixture material id (decimal 9003), the same fixture
    // scene_renderer.cpp cooks. The core pack (auto-mounted by AssetManager)
    // supplies the canonical layout the brick vertex shader references and the
    // lighting/blit shaders the SceneRenderer loads.
    constexpr AssetId BrickMaterialId{0x232BULL};

    int g_Failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++g_Failures;
        }
    }
}

int main()
{
    if (!Test::HasVulkanDriver())
    {
        return 77;
    }

    Time::Initialize();

    // A windowed context: the ImGui Vulkan backend (and so ImGuiTexture's
    // descriptor set) requires a swapchain image count, which is only available
    // with a window. The window is never shown to a frame loop here.
    const Unique<Window> window = Window::Create({
        .Extent = {640, 480},
        .Resizable = false,
        .Title = "Material Preview Test",
        .CaptureMouse = false,
    });

    Context context;
    context.Initialize(
        {
            .ApplicationName = "Material Preview Test",
            .HeadlessExtent = {640, 480},
        },
        window.get());

    // The AssetManager takes a TaskSystem by reference; this test loads through the
    // blocking LoadSync (UploadSync) path, so no transfer pools are wired up
    // (InitializeTransferPools is unneeded — nothing submits async transfers).
    Unique<TaskSystem> tasks = CreateUnique<TaskSystem>(TaskSystemInfo{.WorkerCount = 2});

    Unique<ImGuiLayer> imgui = ImGuiLayer::Create({}, context, *window);

    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);

        AssetManager assets(context, *tasks, types);

        // Cook the brick fixture pack in-process and mount it over the auto-mounted
        // core pack.
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_material_preview.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        // The brick shaders `#include "Veng/material.slang"`; the engine core shader dir is on
        // the cook's Slang search path so the cross-pack include resolves.
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        Check(cookResult.has_value(), "cook brick fixture pack");

        const VoidResult mountResult = assets.Mount(outArchive);
        Check(mountResult.has_value(), "mount brick fixture pack");

        const AssetResult<AssetHandle<MaterialInstance>> brick =
            assets.LoadSync<MaterialInstance>(BrickMaterialId);
        Check(brick.has_value(), "load brick material");

        constexpr uvec2 previewExtent{256, 256};

        VengEditor::MaterialPreview preview(context, assets, *imgui, previewExtent);
        if (brick.has_value())
        {
            preview.SetMaterial(*brick);
        }

        // A second, viewport-style SceneRenderer over its own one-sphere scene — the
        // two-renderer handoff the validation gate must exercise. Disjoint targets,
        // each bracketing its own output.
        constexpr uvec2 viewportExtent{320, 240};
        const Ref<Mesh> viewportSphere = Mesh::BuildSync(
            context,
            Primitives::Icosphere(0.85f, 4,
                                  brick.has_value() ? *brick : AssetHandle<MaterialInstance>{}),
            "Viewport Sphere");

        const Unique<Scene> viewportScene = Scene::Create(types);
        const Entity viewportEntity = viewportScene->CreateEntity();
        viewportScene->Add<Transform>(viewportEntity);
        viewportScene->Add<MeshRenderer>(viewportEntity).Mesh = assets.Adopt(viewportSphere);

        const Entity viewportLight = viewportScene->CreateEntity();
        viewportScene->Add<Light>(viewportLight) = Light{
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f),
            .Intensity = 1.5f,
        };

        CameraView viewportCamera;
        viewportCamera.SetPerspective(glm::radians(45.0f),
                                      static_cast<f32>(viewportExtent.x) / viewportExtent.y, 0.1f,
                                      100.0f);
        viewportCamera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        const Unique<SceneRenderer> viewport = SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = viewportExtent,
            .Settings = {},
        });

        Check(viewport->GetOutput() != nullptr, "viewport output is a valid view");
        Check(viewport->GetOutput()->GetImage()->GetWidth() == viewportExtent.x,
              "viewport output sized to its own extent");

        // Record both renderers in one command stream, each bracketing its own
        // output with PrepareForAccess(Sample) — the two-renderer interaction. The
        // preview's Update pushes its view; the viewport's Render does the Execute +
        // Sample barrier itself, standing in for the engine drive-list.
        preview.Update();
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                viewport->Execute(cmd, Renderer::SceneView{.World = *viewportScene,
                                                           .Camera = viewportCamera,
                                                           .Delta = 0.0f});
                cmd.PrepareForAccess(viewport->GetOutput(), AccessKind::Sample);

                preview.GetViewport().Render(cmd);
            });

        // The preview is sized to the requested 256² extent and exposes a non-null
        // ImGuiTexture.
        Check(preview.GetExtent() == previewExtent, "preview extent is 256²");
        Check(preview.GetTexture()->GetTextureId() != 0, "preview texture id is non-null");

        // A second frame records cleanly: the viewport re-arms its output for the
        // next Execute, so the preview keeps drawing across frames.
        preview.Update();
        context.ImmediateCommands([&](CommandBuffer& cmd) { preview.GetViewport().Render(cmd); });

        Check(preview.GetTexture()->GetTextureId() != 0,
              "preview texture id valid after a second frame");

        std::filesystem::remove(outArchive);
    }

    // Teardown mirrors Application::Dispose: drain in-flight work, then release the
    // ImGui layer and task system before the context disposes its resources, so no
    // GPU resource outlives the context.
    context.WaitIdle();
    imgui.reset();
    tasks.reset();
    context.DisposeResources();
    context.Dispose();

    return g_Failures == 0 ? 0 : 1;
}
