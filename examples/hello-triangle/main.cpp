#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>
#include <Veng/Vendor/ImGui.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>

#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdlib>
#include <fstream>

using namespace Veng;

namespace
{
    // Selects the composite shader's bindless texture/sampler slots
    // (Veng/Renderer/BindlessRegistry.h) — set 0 is bound once via
    // BindlessRegistry::Bind, these indices pick the array elements.
    struct CompositePushConstants
    {
        u32 SceneTexture;
        u32 ImGuiTexture;
        u32 Sampler;
    };
}

// A game-defined component: spins its entity about a fixed axis. The engine has
// no compile-time knowledge of this type — it is registered through the same
// public TypeRegistry::Register<T> path the engine uses for its own builtins,
// proving the scene stores, queries, and (via the reflection layer) serializes
// a type defined entirely in the game's translation unit.
struct Spinner
{
    f32 SpeedRadiansPerSec = 1.0f;
};

VE_REFLECT(Spinner, 0xAEF00D5EFC2444DAULL)
    VE_FIELD(SpeedRadiansPerSec, .DisplayName = "Speed", .Tooltip = "Radians per second", .Min = 0.0)
VE_REFLECT_END();

class HelloTriangleApp final : public Application
{
public:
    HelloTriangleApp(const ApplicationInfo& info, TypeRegistry& types) : Application(info, types)
    {
    }

protected:
    void OnInitialize() override
    {
        auto& context = GetRenderContext();

        const uvec2 sceneExtent = context.GetInternalRenderExtent();

        m_SmokeOutput = std::getenv("HT_SMOKE");

        // The main view renders through one SceneRenderer: it owns its offscreen
        // output (created at the context's output format), the deferred g-buffer,
        // an internal compiled graph, and the deferred draw. It loads its
        // fullscreen-blit shaders from the engine core pack through the asset
        // manager. The sample composites GetOutput() and the smoke path downloads
        // it.
        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = GetAssetManager(),
            .OutputFormat = context.GetOutputFormat(),
            .Extent = sceneExtent,
            .Settings = m_SceneSettings,
        });

        m_Sampler = Renderer::Sampler::Create(context, {
            .Name = "Sample Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        // Cooked at build time (see CMakeLists.txt) and copied beside the launcher
        // by veng_add_game; mount it from the executable's directory so the trio
        // (launcher + module + pack) resolves wherever it is copied. Loading the
        // brick material pulls in its vertex/fragment shaders and the brick texture
        // as eager dependencies, builds its bindless pipeline, and writes a
        // MaterialData entry into the registry's per-material SSBO.
        const VoidResult mountResult = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        // The primitive generator records this material instance on the produced
        // submesh, so it must be resident before Mesh::Create hands it in.
        const AssetResult<AssetHandle<Veng::Material>> brickMaterial =
            GetAssetManager().LoadSync<Veng::Material>(AssetId{0x3EB});
        VE_ASSERT(brickMaterial.has_value(), "{}", brickMaterial.error().Detail);
        m_BrickMaterial = *brickMaterial;

        // Build the geometry at runtime rather than loading a cooked mesh: a
        // geodesic icosphere in the canonical layout, carrying the brick material
        // instance on its single submesh. Its near-uniform tessellation shows the
        // brick UV mapping without the pole clustering of a UV sphere.
        // Mesh::Create uploads synchronously, so it is ready to draw.
        const Ref<Veng::Mesh> sphere = Veng::Mesh::Create(
            context, Veng::Primitives::Icosphere(0.8f, 4, m_BrickMaterial), "Demo Sphere");

        // The compositing path (ImGui overlay + swapchain present) only exists in
        // windowed mode. The headless smoke run renders just the scene and
        // downloads it — no ImGui layer, no swapchain.
        if (GetImGuiLayer())
        {
            m_ImGuiImageView = Renderer::ImageView::Create(context, {
                .Name = "Sample ImGui Image View",
                .Image = GetImGuiLayer()->GetOutputImage(),
            });

            // CreateCompositePipeline registers the scene output (bindless slot + the
            // ImGui scene texture) via RegisterSceneOutput.
            CreateCompositePipeline();

            // A swapchain resize invalidates the composite pass's baked extent;
            // rebuild + re-Compile() the composite graph against the new size. The
            // SceneRenderer keeps a fixed internal extent, so its output view stays
            // valid and is not re-registered. The headless smoke path has a fixed
            // extent, so this never fires there.
            context.AddSwapChainInvalidationCallback([this]
            {
                m_CompositeGraph = BuildCompositeGraph();
            });
        }

        // Build the runtime scene by spawning a cooked prefab: it carries the
        // entity's local Transform, a MeshRenderer (its Mesh field "no asset",
        // since a runtime primitive has no content identity), and the game-defined
        // Spinner. The Scene is an engine primitive — created empty and populated
        // by spawning, never loaded.
        m_Scene = Scene::Create(GetTypeRegistry());

        const AssetResult<AssetHandle<Veng::Prefab>> prefab =
            GetAssetManager().LoadSync<Veng::Prefab>(AssetId{0xA123F30FD219F2D5ULL});
        VE_ASSERT(prefab.has_value(), "{}", prefab.error().Detail);

        const vector<Entity> roots = prefab->Get()->SpawnInto(*m_Scene, GetAssetManager());
        VE_ASSERT(!roots.empty(), "prefab spawned no root entities");

        // Adopt the runtime mesh into an AssetHandle and assign it to the spawned
        // renderer — the one piece wired in code, because the prefab cannot
        // reference a runtime resource by id. The adopted handle owns the mesh's
        // residency; dropping the scene drops the component, the handle, and the
        // mesh in turn.
        m_Scene->Get<MeshRenderer>(roots[0]).Mesh = GetAssetManager().Adopt(sphere);

        // A directional light so the deferred lighting pass shades the scene. Its
        // direction is fixed (top-front-right toward the origin), so the smoke pose
        // is lit reproducibly run to run.
        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 1.5f,
        };

        // A Camera looking down -Z at the origin from (0,0,3).
        const f32 aspect = static_cast<f32>(sceneExtent.x) / static_cast<f32>(sceneExtent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        m_Camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        // The composite graph is compiled once and replayed every frame; a swapchain
        // resize re-Compile()s it via the invalidation callback above. The
        // SceneRenderer compiles its own graph in Create().
        if (GetImGuiLayer())
            m_CompositeGraph = BuildCompositeGraph();
    }

    void OnUpdate(const f32 delta) override
    {
        m_LastDelta = delta;

        // Drive rotation through the scene query: each spinning entity advances
        // its Transform's quaternion about the fixed axis by its speed * delta.
        // Smoke mode pins the fixed pose so the capture is reproducible run to
        // run and can be golden-compared; the windowed app rotates by
        // accumulated wall-clock delta.
        if (m_SmokeOutput)
        {
            m_Scene->Each<Transform, Spinner>([](Entity, Transform& transform, Spinner&)
            {
                transform.Rotation = glm::angleAxis(SmokeAngle, SpinAxis);
            });
        }
        else
        {
            m_Scene->Each<Transform, Spinner>([delta](Entity, Transform& transform, Spinner& spinner)
            {
                const quat step = glm::angleAxis(spinner.SpeedRadiansPerSec * delta, SpinAxis);
                transform.Rotation = glm::normalize(step * transform.Rotation);
            });
        }

        // Smoke-test mode: after a few rendered frames, dump the scene image to
        // disk and exit. Runs before this frame's commands are recorded, so the
        // image holds the previous frame's completed contents.
        if (m_SmokeOutput && ++m_FrameCount == 20)
        {
            WriteSceneCapture(m_SmokeOutput);
            RequestExit();
        }
    }

    void OnRender() override
    {
        auto& context = GetRenderContext();
        auto& cmd = context.GetCurrentCommandBuffer();

        const Renderer::SceneView view{.World = *m_Scene, .Camera = m_Camera, .Delta = m_LastDelta};
        m_SceneRenderer->Execute(cmd, view);

        // Headless (smoke) renders only the scene; the ImGui overlay and the
        // composite-to-swapchain pass are windowed-only.
        if (GetImGuiLayer())
        {
            // RenderUserInterface() draws m_SceneTexture via ImGui::Image(), and
            // GetImGuiLayer()->Render(cmd) is what actually records that sampled
            // read — before the composite pass's own .Sample() declaration runs.
            // ImGui samples outside the graph, so declare the read explicitly:
            // transition the scene output to a sampleable layout before that pass.
            cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);

            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        // The SceneRenderer owns the scene output + depth transient; the composite
        // graph owns its imports' bindings. Release them so their GPU resources
        // retire before the context tears down.
        m_SceneRenderer.reset();
        m_CompositeGraph.reset();
        m_Scene.reset();
        m_BrickMaterial = {};
        m_SceneTexture.reset();
        m_CompositePipeline.reset();
        m_CompositeLayout.reset();
        m_CompositeVS = {};
        m_CompositeFS = {};
        m_Sampler.reset();
        m_ImGuiImageView.reset();
    }

private:
    void CreateCompositePipeline()
    {
        auto& context = GetRenderContext();

        const AssetResult<AssetHandle<Veng::Shader>> vs =
            GetAssetManager().LoadSync<Veng::Shader>(AssetId{0x3EE});
        VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
        m_CompositeVS = *vs;

        const AssetResult<AssetHandle<Veng::Shader>> fs =
            GetAssetManager().LoadSync<Veng::Shader>(AssetId{0x3EF});
        VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
        m_CompositeFS = *fs;

        m_CompositeLayout = Renderer::PipelineLayout::Create(context, {
            .Name = "Composite Layout",
            .PushConstantRanges = {
                Renderer::PushConstantRange::Of<CompositePushConstants>(Renderer::ShaderStage::Fragment),
            },
        });

        m_CompositePipeline = Renderer::GraphicsPipeline::Create(context, {
            .Name = "Composite Pipeline",
            // The composite pass renders into the swapchain image, which the
            // context owns and already exposes a single format accessor — no
            // separate Image::Create to keep in sync with here.
            .ColorAttachments = {{.Format = context.GetSwapChainFormat()}},
            .PipelineLayout = m_CompositeLayout,
            .ShaderStages = {
                {.Stage = Renderer::ShaderStage::Vertex, .Module = m_CompositeVS.Get()->Module},
                {.Stage = Renderer::ShaderStage::Fragment, .Module = m_CompositeFS.Get()->Module},
            },
        });

        // Register the scene/ImGui views and the shared sampler into the
        // bindless registry (set 0) — composite.frag indexes them via push
        // constants.
        auto& bindless = context.GetBindlessRegistry();
        m_ImGuiTextureHandle = bindless.Register(m_ImGuiImageView);
        m_SamplerHandle = bindless.Register(m_Sampler);
        RegisterSceneOutput();
    }

    // (Re-)register the SceneRenderer's current output into the bindless slot the
    // composite samples and the ImGui texture the Scene panel draws. Called once at
    // setup and again after Configure (which recreates the output image, invalidating
    // the prior slot/texture). The composite reads m_SceneTextureHandle.Index live
    // per frame, so updating the handle takes effect on the next replay.
    void RegisterSceneOutput()
    {
        auto& bindless = GetRenderContext().GetBindlessRegistry();
        if (m_SceneTextureHandle.IsValid())
            bindless.Release(m_SceneTextureHandle);
        m_SceneTextureHandle = bindless.Register(m_SceneRenderer->GetOutput());
        m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_Sampler, *m_SceneRenderer->GetOutput());
    }

    void RenderUserInterface()
    {
        ImGui::Begin("Scene");

        // The DebugView combo drives SceneRenderer::Configure, re-wiring the pass set
        // (Final / Albedo / Normal / Depth) — a live exercise of the recompile seam.
        // Configure recreates the output image, so the cached scene texture + bindless
        // slot must be re-built after it (the GetOutput()-invalidated-by-Configure
        // contract).
        const char* modeNames[] = {"Final", "Albedo", "Normal", "Depth"};
        int mode = static_cast<int>(m_SceneSettings.Mode);
        if (ImGui::Combo("View", &mode, modeNames, IM_ARRAYSIZE(modeNames)))
        {
            m_SceneSettings.Mode = static_cast<Renderer::DebugView>(mode);
            m_SceneRenderer->Configure(m_SceneSettings);
            RegisterSceneOutput();
        }

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const Ref<Renderer::ImageView> output = m_SceneRenderer->GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetHeight()) /
                           static_cast<f32>(output->GetImage()->GetWidth());
        ImGui::Image(static_cast<ImTextureID>(m_SceneTexture->GetTextureId()),
                     {available.x, available.x * aspect});
        ImGui::End();

        ImGui::Begin("Stats");
        ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::End();
    }

    void WriteSceneCapture(const char* outPath) const
    {
        const Ref<Renderer::Image> output = m_SceneRenderer->GetOutput()->GetImage();
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
                const f32 value = glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                out.put(static_cast<char>(value * 255.0f + 0.5f));
            }
        }

        Log::Info("Wrote scene capture to {}", outPath);
    }

    // Build and compile the composite graph. Compiled once and replayed every
    // frame; the callback closes over only `this` and reads the swapchain extent
    // live (fixed for a given swapchain — a resize re-Compile()s the graph).
    Unique<Renderer::CompiledGraph> BuildCompositeGraph()
    {
        auto& context = GetRenderContext();

        // Sampling the scene and ImGui views declares the reads that drive their
        // transitions to ShaderReadOnly; rendering the swapchain view declares
        // the write that drives its transition to ColorAttachment. All three are
        // app-/context-owned, so they are imports; the swapchain view differs each
        // frame and is supplied per call.
        Renderer::RenderGraph graph(context);
        m_SwapId = graph.Import("SwapChain");
        m_CompositeSceneId = graph.Import("Scene");
        m_ImGuiId = graph.Import("ImGui");
        graph.AddPass("Composite")
            .Color({
                .Resource = m_SwapId,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(m_CompositeSceneId)
            .Sample(m_ImGuiId)
            .Execute([this](Renderer::PassContext& ctx)
            {
                Renderer::CommandBuffer& cmd = ctx.Cmd();
                const uvec2 extent = GetRenderContext().GetSwapChainExtent();
                cmd.BindPipeline(m_CompositePipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                GetRenderContext().GetBindlessRegistry().Bind(cmd);
                cmd.PushConstants(CompositePushConstants{
                    .SceneTexture = m_SceneTextureHandle.Index,
                    .ImGuiTexture = m_ImGuiTextureHandle.Index,
                    .Sampler = m_SamplerHandle.Index,
                });
                cmd.DrawFullscreenTriangle();
            });

        return graph.Compile();
    }

    void CompositeToSwapChain(Renderer::CommandBuffer& cmd)
    {
        const Renderer::RenderGraph::ImportBinding bindings[] = {
            {m_SwapId, GetRenderContext().GetCurrentSwapChainImageView()},
            {m_CompositeSceneId, m_SceneRenderer->GetOutput()},
            {m_ImGuiId, m_ImGuiImageView},
        };
        m_CompositeGraph->Execute(cmd, bindings);
    }

    Unique<Renderer::SceneRenderer> m_SceneRenderer;
    Renderer::SceneRendererSettings m_SceneSettings;
    Ref<Renderer::ImageView> m_ImGuiImageView;
    Ref<Renderer::Sampler> m_Sampler;

    AssetHandle<Veng::Material> m_BrickMaterial;

    AssetHandle<Veng::Shader> m_CompositeVS;
    AssetHandle<Veng::Shader> m_CompositeFS;
    Ref<Renderer::PipelineLayout> m_CompositeLayout;
    Ref<Renderer::GraphicsPipeline> m_CompositePipeline;
    Renderer::TextureHandle m_SceneTextureHandle;
    Renderer::TextureHandle m_ImGuiTextureHandle;
    Renderer::SamplerHandle m_SamplerHandle;

    Ref<ImGuiTexture> m_SceneTexture;

    // Compiled once and replayed every frame; re-Compile()d on swapchain resize.
    Unique<Renderer::CompiledGraph> m_CompositeGraph;

    // Import slots bound per frame to Execute. Stable across replays; only the
    // concrete views they bind to change.
    Renderer::ResourceId m_SwapId;
    Renderer::ResourceId m_CompositeSceneId;
    Renderer::ResourceId m_ImGuiId;

    // The fixed rotation the smoke capture renders, in radians.
    static constexpr f32 SmokeAngle = 0.9f;
    // The axis the spinner rotates about — the same axis the prior hand-rolled
    // model matrix used, so the rendered orientation is unchanged.
    static inline const vec3 SpinAxis = glm::normalize(vec3(0.5f, 1.0f, 0.2f));

    Unique<Scene> m_Scene;
    Camera m_Camera;

    f32 m_LastDelta = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

// The module's entry point: the launcher dlopens this library and calls this
// once, and the game registers the factory that constructs its Application. The
// factory captures the HT_SMOKE/headless decision and the ApplicationInfo, so
// they live in the module beside the app — the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The game registers its own component types here, through the same public
    // path the engine pre-registers its builtins (already present on the host
    // registry). The engine has no compile-time knowledge of Spinner.
    host->Types.Register<Spinner>();

    // Smoke mode runs headless: no window or swapchain, render the scene
    // off-screen and dump it. This is the display-free CI path enabled by the
    // headless context.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    host->App.RegisterApplication([smoke](TypeRegistry& types)
    {
        return Unique<Application>(new HelloTriangleApp(ApplicationInfo{
            .Name = "Hello Triangle",
            .InternalRenderExtent = {1280, 720},
            .WindowInfo = {
                .Extent = {1280, 720},
                .Resizable = false,
                .EventCallback = [](Event&) {},
                .Title = "veng — Hello Triangle",
                .CaptureMouse = false,
            },
            .Headless = smoke,
            // Persist the pipeline cache beside the launcher — the same
            // executable-relative resolution the asset pack uses, so the cache
            // stays with the relocatable trio.
            .PipelineCachePath = ExecutableDirectory() / "pipeline_cache.bin",
        }, types));
    });
}

VE_EXPORT_MODULE_ABI()
