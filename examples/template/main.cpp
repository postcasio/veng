#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cstdlib>
#include <fstream>
#include <utility>

using namespace Veng;

// The smallest veng game: open a window and draw one lit, rotating cube. The world is three
// entities authored in code — a camera, a directional light, and the cube — with no custom
// component, system, prefab, or debug UI. Copy this directory to start a new veng game; see
// examples/hello-triangle and the editor's Project Settings panel for the richer surface.
class TemplateApp final : public Application
{
public:
    TemplateApp(const ApplicationInfo& info, TypeRegistry& types, SystemRegistry& systems)
        : Application(info, types, systems)
    {
    }

protected:
    void OnInitialize() override
    {
        m_SmokeOutput = std::getenv("TEMPLATE_SMOKE");

        // Executable-relative so the pack resolves wherever the launcher is copied.
        const VoidResult mounted =
            GetAssetManager().Mount(ExecutableDirectory() / "template.vengpack");
        VE_ASSERT(mounted, "{}", mounted.error());

        // The cube's flat surface material, resident before the cube mesh records it.
        const AssetResult<AssetHandle<Material>> material =
            GetAssetManager().LoadSync<Material>(AssetId{0x39D27BFE56BC955BULL});
        VE_ASSERT(material.has_value(), "{}", material.error().Detail);

        m_Scene = Scene::Create(GetTypeRegistry());

        // The camera: pulled back and slightly above, looking at the origin where the cube sits.
        const Entity camera = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(camera, Transform{.Position = vec3(0.0f, 1.5f, 4.0f)});
        m_Scene->Add<Camera>(camera);
        // Aim the camera at the origin; the view derives from the entity's world matrix.
        m_Scene->Get<Transform>(camera).Rotation = glm::quatLookAt(
            glm::normalize(vec3(0.0f) - vec3(0.0f, 1.5f, 4.0f)), vec3(0.0f, 1.0f, 0.0f));

        // A single directional light from front-above so the cube's faces read distinctly.
        const Entity light = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(light);
        m_Scene->Add<Light>(light, Light{
                                       .Type = LightType::Directional,
                                       .Direction = glm::normalize(vec3(-0.4f, -1.0f, -0.6f)),
                                       .Color = vec3(1.0f),
                                       .Intensity = 3.0f,
                                   });

        // The cube: a primitive recipe built into a resident mesh in code — no cooked mesh asset,
        // no .obj. BuildSync blocks on the upload + bindless registration so the first frame draws
        // it; the submesh records the flat material.
        MeshData geometry = Primitives::Cube(1.0f, material.value());
        m_Cube = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(m_Cube);
        m_Scene->Add<MeshRenderer>(m_Cube, MeshRenderer{.Mesh = GetAssetManager().BuildSync<Mesh>(
                                                            std::move(geometry), "Cube")});
    }

    void OnUpdate(const f32 delta) override
    {
        // Rotate the cube about its Y axis: the minimal "something is alive on screen". Smoke
        // mode pins a fixed angle so the capture is reproducible; the windowed app accumulates.
        m_Angle = m_SmokeOutput ? SmokeAngle : m_Angle + delta;
        m_Scene->Get<Transform>(m_Cube).Rotation = glm::angleAxis(m_Angle, vec3(0.0f, 1.0f, 0.0f));

        // Push this frame's render source into the engine-owned managed viewport: the resolved
        // camera at the output's aspect plus the scene. The engine renders the viewport before
        // OnRender, so nothing else is needed to put the cube on screen.
        const Ref<Renderer::ImageView> output = GetPrimaryViewport()->GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());

        const optional<CameraView> camera = ResolvePrimaryCameraView(*m_Scene, aspect);
        VE_ASSERT(camera.has_value(), "scene resolves no camera");

        Renderer::ViewState view;
        view.World = m_Scene.get();
        view.Camera = *camera;
        view.Delta = delta;
        GetPrimaryViewport()->SetViewState(view);

        // Smoke: capture after a few frames so the cube's streamed upload has landed, then exit.
        if (m_SmokeOutput && ++m_FrameCount == 4)
        {
            WriteSceneCapture(m_SmokeOutput);
            RequestExit();
        }
    }

    void OnDispose() override { m_Scene.reset(); }

private:
    // Fixed rotation for the headless capture, in radians.
    static constexpr f32 SmokeAngle = 0.6f;

    void WriteSceneCapture(const char* outPath) const
    {
        const Ref<Renderer::Image> output = GetPrimaryViewport()->GetOutput()->GetImage();
        const auto data = output->Download();
        const u32 width = output->GetWidth();
        const u32 height = output->GetHeight();

        // Scene output is RGBA16F; decode to 8-bit RGB for a binary PPM.
        const auto* halves = reinterpret_cast<const u16*>(data.data());

        std::ofstream out(outPath, std::ios::binary);
        out << "P6\n" << width << " " << height << "\n255\n";
        for (u32 pixel = 0; pixel < width * height; pixel++)
        {
            for (u32 channel = 0; channel < 3; channel++)
            {
                const f32 value =
                    glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                out.put(static_cast<char>(value * 255.0f + 0.5f));
            }
        }

        Log::Info("Wrote scene capture to {}", outPath);
    }

    Unique<Scene> m_Scene;
    Entity m_Cube = Entity::Null;
    f32 m_Angle = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // Headless when TEMPLATE_SMOKE is set: render off-screen and dump, the display-free CI path.
    const bool smoke = std::getenv("TEMPLATE_SMOKE") != nullptr;

    host->App.RegisterApplication(
        [smoke](TypeRegistry& types, SystemRegistry& systems)
        {
            return Unique<Application>(new TemplateApp(
                ApplicationInfo{
                    .Name = "Template",
                    .HeadlessExtent = {1280, 720},
                    .WindowInfo =
                        {
                            .Extent = {1280, 720},
                            .Title = "veng — Template",
                        },
                    // The engine owns the primary viewport (its SceneRenderer + the gather +
                    // composite tail); the app pushes only a ViewState each frame.
                    .ManagedViewport = ManagedViewportInfo{},
                    .Headless = smoke,
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
