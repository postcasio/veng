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

#include "AssetSourceIndex.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/InspectorPanel.h"
#include "panels/MaterialEditorPanel.h"
#include "panels/SceneViewportPanel.h"
#include "panels/TextureEditorPanel.h"

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

        // The built-in texture editor factory: resolves a texture AssetId to its
        // .tex.json source through the shared manifest index, then mints a
        // TextureEditorPanel wired to the host's engine refs and cook driver.
        class TextureEditorFactory final : public AssetEditorFactory
        {
        public:
            TextureEditorFactory(const AssetSourceIndex& index, Renderer::Context& context,
                                 AssetManager& assets, ImGuiLayer& imgui, VengEditor::CookDriver cook) :
                m_Index(index), m_Context(context), m_Assets(assets),
                m_ImGui(imgui), m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Texture editor: no source manifest entry for asset 0x{:X}", id.Value);
                    return nullptr;
                }

                return CreateUnique<TextureEditorPanel>(
                    id, entry->Source, m_Context, m_Assets, m_ImGui, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            VengEditor::CookDriver m_Cook;
        };

        // The built-in material editor factory: resolves a material AssetId to its
        // .vmat.json source through the shared manifest index, then mints a
        // MaterialEditorPanel wired to the host's engine refs, the source index
        // (the asset picker's candidate enumeration), and the cook driver.
        class MaterialEditorFactory final : public AssetEditorFactory
        {
        public:
            MaterialEditorFactory(const AssetSourceIndex& index, Renderer::Context& context,
                                  AssetManager& assets, ImGuiLayer& imgui, EditorRegistry& editors,
                                  VengEditor::CookDriver cook) :
                m_Index(index), m_Context(context), m_Assets(assets),
                m_ImGui(imgui), m_Editors(editors), m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Material editor: no source manifest entry for asset 0x{:X}", id.Value);
                    return nullptr;
                }

                return CreateUnique<MaterialEditorPanel>(
                    id, entry->Source, m_Index, m_Context, m_Assets, m_ImGui, m_Editors, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            EditorRegistry& m_Editors;
            VengEditor::CookDriver m_Cook;
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

        // The shared source index (AssetId -> per-asset JSON source), parsed once
        // and referenced by the asset-editor factories and the inspector's asset
        // picker. An empty index when no manifest is configured keeps the picker
        // candidate-free rather than absent.
        m_Sources = CreateUnique<AssetSourceIndex>(
            m_Info.AssetManifestPath ? AssetSourceIndex::Parse(*m_Info.AssetManifestPath)
                                     : AssetSourceIndex{});

        // The built-in asset editors: registered before the modules' own editor
        // factories would be (first-write-wins), so a game module registering its
        // own editor for the same type does not override the built-in. The cook
        // driver is RequestCook bound to this host.
        if (m_Info.AssetManifestPath)
        {
            auto cookFor = [this] {
                return VengEditor::CookDriver([this](const VengEditor::CookRequest& request,
                                                     function<void(Result<MountHandle>)> onComplete)
                {
                    RequestCook(request, std::move(onComplete));
                });
            };

            m_Registries->Editor.RegisterAssetEditor(AssetType::Texture,
                CreateUnique<TextureEditorFactory>(
                    *m_Sources, GetRenderContext(), GetAssetManager(), *GetImGuiLayer(), cookFor()));

            m_Registries->Editor.RegisterAssetEditor(AssetType::Material,
                CreateUnique<MaterialEditorFactory>(
                    *m_Sources, GetRenderContext(), GetAssetManager(), *GetImGuiLayer(),
                    m_Registries->Editor, cookFor()));
        }

        auto assetBrowser = CreateUnique<AssetBrowserPanel>(
            ExecutableDirectory() / "sample.vengpack", m_Registries->Editor);
        m_AssetBrowser = assetBrowser.get();
        m_Panels.push_back({std::move(assetBrowser), true});
        auto inspector = CreateUnique<InspectorPanel>(GetAssetManager(), m_Registries->Editor, *m_Sources);
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

    void EditorHost::RequestCook(const CookRequest& request,
                                 function<void(Result<MountHandle>)> onComplete)
    {
        if (!m_Info.Cook)
        {
            const string error = "editor: cook-on-demand backend not configured";
            Log::Error("{}", error);
            onComplete(std::unexpected(error));
            return;
        }

        Task<vector<u8>> task = m_Info.Cook(request, GetTaskSystem());

        // The continuation runs on the main thread via the task system's pump, so
        // the shadow-mount and the callback land on the render thread, where the
        // AssetManager lives.
        task.Then([this, targetId = request.TargetId, source = request.SourcePath,
                   onComplete = std::move(onComplete)](Result<vector<u8>> bytes) mutable
        {
            if (!bytes)
            {
                Log::Error("editor: cook of '{}' failed: {}", source.string(), bytes.error());
                onComplete(std::unexpected(bytes.error()));
                return;
            }

            MountHandle handle = GetAssetManager().MountMemory(
                std::move(*bytes), fmt::format("cook:{}", targetId.Value));
            onComplete(std::move(handle));
        });
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
                    const string title(slot.Panel->GetTitle());
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

        // Adopt any asset-editor panels the browser opened since last frame.
        for (Unique<EditorPanel>& opened : m_AssetBrowser->TakeOpenedPanels())
            m_Panels.push_back({std::move(opened), true});

        // Drive each open panel's offscreen render (a material editor's preview)
        // before the ImGui frame is built, so its output is sampleable. The
        // viewport is driven explicitly above; its OnRender is a no-op.
        for (PanelSlot& slot : m_Panels)
            if (slot.Open)
                slot.Panel->OnRender(cmd);

        ImGui::DockSpaceOverViewport();
        DrawMenuBar();

        for (PanelSlot& slot : m_Panels)
        {
            if (!slot.Open)
                continue;

            const string title(slot.Panel->GetTitle());
            const ImGuiWindowFlags flags = slot.Panel->GetWindowFlags();
            const bool noPadding = (flags & ImGuiWindowFlags_NoScrollbar) != 0;
            if (noPadding)
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            const bool open = ImGui::Begin(title.c_str(), &slot.Open, flags);
            if (noPadding)
                ImGui::PopStyleVar();
            if (open)
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
        m_AssetBrowser = nullptr;
        m_Sources.reset();
        m_PresentGraph.reset();
        m_BlitPipeline.reset();
        m_BlitLayout.reset();
        m_BlitVS = {};
        m_BlitFS = {};
        m_Sampler.reset();
        m_ImGuiView.reset();
    }
}
