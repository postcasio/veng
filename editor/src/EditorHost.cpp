#include <VengEditor/EditorHost.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Vendor/ImGui.h>

#include "panels/AssetBrowserPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/InspectorPanel.h"
#include "panels/SceneViewportPanel.h"

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The core pack's fullscreen vertex stage and the single-texture blit the
        // host uses to present the ImGui output to the swapchain.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId BlitFragId{0xBEB6DB78DFCF1D33ULL};

        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };
    }

    // Owns the host-side registries so the base Application can borrow the
    // TypeRegistry. Constructed before EditorHost in Create, so the reference the
    // base stores is valid.
    struct EditorHost::Registries
    {
        ApplicationRegistry App;
        TypeRegistry Types;
        EditorRegistry Editor;
    };

    Unique<EditorHost> EditorHost::Create(const EditorHostInfo& info)
    {
        auto registries = CreateUnique<Registries>();

        RegisterBuiltinTypes(registries->Types);

        Result<LoadedModule> gameModule = ModuleLoader::Load(info.GameModulePath);
        VE_ASSERT(gameModule.has_value(), "editor: failed to load game module {}: {}",
                  info.GameModulePath.string(), gameModule.error());

        optional<LoadedModule> editorModule;
        if (info.EditorModulePath)
        {
            Result<LoadedModule> loaded = ModuleLoader::Load(*info.EditorModulePath);
            VE_ASSERT(loaded.has_value(), "editor: failed to load editor module {}: {}",
                      info.EditorModulePath->string(), loaded.error());
            editorModule.emplace(std::move(*loaded));
        }

        VengModuleHost host{
            .App = registries->App,
            .Types = registries->Types,
            .Editor = &registries->Editor,
        };
        gameModule->Register(host);
        if (editorModule)
            editorModule->Register(host);

        auto gameModulePtr = CreateUnique<LoadedModule>(std::move(*gameModule));
        return Unique<EditorHost>(new EditorHost(info, std::move(registries),
                                                 std::move(gameModulePtr), std::move(editorModule)));
    }

    EditorHost::EditorHost(const EditorHostInfo& info, Unique<Registries> registries,
                           Unique<LoadedModule> gameModule, optional<LoadedModule> editorModule) :
        Application(info.App, registries->Types),
        m_Info(info),
        m_Registries(std::move(registries)),
        m_GameModule(std::move(gameModule)),
        m_EditorModule(std::move(editorModule))
    {
    }

    EditorHost::~EditorHost() = default;

    void EditorHost::OnInitialize()
    {
        VE_ASSERT(GetImGuiLayer() != nullptr, "editor host requires the ImGui layer");

        const VoidResult mount = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mount, "{}", mount.error());

        // The blit pipeline that presents the ImGui output to the swapchain.
        {
            const AssetResult<AssetHandle<Shader>> vs = GetAssetManager().LoadSync<Shader>(FullscreenVertId);
            VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
            const AssetResult<AssetHandle<Shader>> fs = GetAssetManager().LoadSync<Shader>(BlitFragId);
            VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
            m_BlitVS = *vs;
            m_BlitFS = *fs;

            m_BlitLayout = Renderer::PipelineLayout::Create(GetRenderContext(), {
                .Name = "Editor Blit Layout",
                .PushConstantRanges = {
                    Renderer::PushConstantRange::Of<BlitPushConstants>(Renderer::ShaderStage::Fragment),
                },
            });

            m_BlitPipeline = Renderer::GraphicsPipeline::Create(GetRenderContext(), {
                .Name = "Editor Blit Pipeline",
                .ColorAttachments = {{.Format = GetRenderContext().GetSwapChainFormat()}},
                .PipelineLayout = m_BlitLayout,
                .ShaderStages = {
                    {.Stage = Renderer::ShaderStage::Vertex, .Module = m_BlitVS.Get()->Module},
                    {.Stage = Renderer::ShaderStage::Fragment, .Module = m_BlitFS.Get()->Module},
                },
            });
        }

        m_Sampler = Renderer::Sampler::Create(GetRenderContext(), {
            .Name = "Editor Present Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        m_ImGuiView = Renderer::ImageView::Create(GetRenderContext(), {
            .Name = "Editor ImGui View",
            .Image = GetImGuiLayer()->GetOutputImage(),
        });

        auto& bindless = GetRenderContext().GetBindlessRegistry();
        m_ImGuiHandle = bindless.Register(m_ImGuiView);
        m_SamplerHandle = bindless.Register(m_Sampler);

        m_PresentGraph = BuildPresentGraph();
        GetRenderContext().AddSwapChainInvalidationCallback([this]
        {
            m_PresentGraph = BuildPresentGraph();
        });

        // The scene viewport panel records the scene render itself, so the host
        // keeps a direct handle to drive it before the panels are drawn.
        auto viewport = CreateUnique<SceneViewportPanel>(
            GetRenderContext(), GetAssetManager(), *GetImGuiLayer(), GetTypeRegistry());
        m_Viewport = viewport.get();
        m_Panels.push_back({std::move(viewport), true});

        m_Panels.push_back({CreateUnique<AssetBrowserPanel>(
            ExecutableDirectory() / "sample.vengpack", m_Registries->Editor), true});
        auto inspector = CreateUnique<InspectorPanel>(GetAssetManager(), m_Registries->Editor);
        m_Inspector = inspector.get();
        m_Panels.push_back({std::move(inspector), true});
        m_Panels.push_back({CreateUnique<ConsolePanel>(), true});

        for (Unique<EditorPanel>& panel : m_Registries->Editor.Panels())
            m_Panels.push_back({std::move(panel), true});
    }

    Unique<Renderer::CompiledGraph> EditorHost::BuildPresentGraph()
    {
        Renderer::RenderGraph graph(GetRenderContext());
        m_SwapId = graph.Import("SwapChain");
        m_ImGuiId = graph.Import("ImGui");
        graph.AddPass("Present")
            .Color({
                .Resource = m_SwapId,
                .Load = Renderer::LoadOp::Clear,
                .Store = Renderer::StoreOp::Store,
                .Clear = Renderer::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(m_ImGuiId)
            .Execute([this](Renderer::PassContext& ctx)
            {
                Renderer::CommandBuffer& cmd = ctx.Cmd();
                const uvec2 extent = GetRenderContext().GetSwapChainExtent();
                cmd.BindPipeline(m_BlitPipeline);
                cmd.SetViewport({0, 0}, extent);
                cmd.SetScissor({0, 0}, extent);
                GetRenderContext().GetBindlessRegistry().Bind(cmd);
                cmd.PushConstants(BlitPushConstants{
                    .Texture = m_ImGuiHandle.Index,
                    .Sampler = m_SamplerHandle.Index,
                });
                cmd.DrawFullscreenTriangle();
            });

        return graph.Compile();
    }

    void EditorHost::DrawMenuBar()
    {
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Exit"))
                    RequestExit();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window"))
            {
                for (PanelSlot& slot : m_Panels)
                {
                    const string title(slot.Panel->Title());
                    ImGui::MenuItem(title.c_str(), nullptr, &slot.Open);
                }
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }
    }

    void EditorHost::OnRender()
    {
        auto& cmd = GetRenderContext().GetCurrentCommandBuffer();

        // The viewport records its scene render before the dockspace UI is built,
        // so its output is sampleable when ImGui draws it into the viewport panel.
        m_Viewport->Render(cmd);

        // Feed the inspector the viewport's live scene and selection. With no
        // hierarchy panel yet, the viewport's prefab root is the default selection.
        m_Inspector->SetSelection(&m_Viewport->GetScene(), m_Viewport->PrimaryEntity());

        ImGui::DockSpaceOverViewport();
        DrawMenuBar();

        for (PanelSlot& slot : m_Panels)
        {
            if (!slot.Open)
                continue;

            const string title(slot.Panel->Title());
            if (ImGui::Begin(title.c_str(), &slot.Open))
                slot.Panel->OnImGui();
            ImGui::End();
        }

        GetImGuiLayer()->Render(cmd);

        const Renderer::RenderGraph::ImportBinding bindings[] = {
            {m_SwapId, GetRenderContext().GetCurrentSwapChainImageView()},
            {m_ImGuiId, m_ImGuiView},
        };
        m_PresentGraph->Execute(cmd, bindings);
    }

    void EditorHost::OnDispose()
    {
        m_Panels.clear();
        m_Viewport = nullptr;
        m_Inspector = nullptr;
        m_PresentGraph.reset();
        m_BlitPipeline.reset();
        m_BlitLayout.reset();
        m_BlitVS = {};
        m_BlitFS = {};
        m_Sampler.reset();
        m_ImGuiView.reset();
    }
}
