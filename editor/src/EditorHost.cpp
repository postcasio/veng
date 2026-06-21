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
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>

#include "AssetSourceIndex.h"
#include "panels/AssetBrowserPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/MaterialEditorPanel.h"
#include "panels/PrefabEditorPanel.h"
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

        // The hello-triangle pack's sphere prefab, opened as the initial document.
        constexpr AssetId SampleScenePrefabId{0xA123F30FD219F2D5ULL};

        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };

        // Resolves a texture AssetId to its .tex.json source through the manifest
        // index, then opens a TextureEditorPanel wired to the host's engine refs.
        class TextureEditorFactory final : public AssetEditorFactory
        {
        public:
            TextureEditorFactory(const AssetSourceIndex& index, Renderer::Context& context,
                                 AssetManager& assets, ImGuiLayer& imgui,
                                 VengEditor::CookDriver cook)
                : m_Index(index), m_Context(context), m_Assets(assets), m_ImGui(imgui),
                  m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Texture editor: no source manifest entry for asset 0x{:X}",
                               id.Value);
                    return nullptr;
                }

                return CreateUnique<TextureEditorPanel>(id, entry->Source, m_Context, m_Assets,
                                                        m_ImGui, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            VengEditor::CookDriver m_Cook;
        };

        // Resolves a material AssetId to its .vmat.json source through the manifest
        // index, then opens a MaterialEditorPanel wired to the host's engine refs.
        class MaterialEditorFactory final : public AssetEditorFactory
        {
        public:
            MaterialEditorFactory(const AssetSourceIndex& index, Renderer::Context& context,
                                  AssetManager& assets, ImGuiLayer& imgui, EditorRegistry& editors,
                                  VengEditor::CookDriver cook)
                : m_Index(index), m_Context(context), m_Assets(assets), m_ImGui(imgui),
                  m_Editors(editors), m_Cook(std::move(cook))
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                const AssetSourceIndex::Entry* entry = m_Index.Find(id);
                if (!entry)
                {
                    Log::Error("Material editor: no source manifest entry for asset 0x{:X}",
                               id.Value);
                    return nullptr;
                }

                return CreateUnique<MaterialEditorPanel>(id, entry->Source, m_Index, m_Context,
                                                         m_Assets, m_ImGui, m_Editors, m_Cook);
            }

        private:
            const AssetSourceIndex& m_Index;
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            EditorRegistry& m_Editors;
            VengEditor::CookDriver m_Cook;
        };

        // Opens a PrefabEditorPanel that spawns the prefab into a live Scene for editing.
        // Needs no manifest source: the prefab is edited in-scene, not recooked.
        class PrefabEditorFactory final : public AssetEditorFactory
        {
        public:
            PrefabEditorFactory(Renderer::Context& context, AssetManager& assets, ImGuiLayer& imgui,
                                TypeRegistry& types, EditorRegistry& editors,
                                const AssetSourceIndex& sources, Input& input,
                                SystemRegistry& systems)
                : m_Context(context), m_Assets(assets), m_ImGui(imgui), m_Types(types),
                  m_Editors(editors), m_Sources(sources), m_Input(input), m_Systems(systems)
            {
            }

            [[nodiscard]] Unique<EditorPanel> OpenEditor(AssetId id) override
            {
                return CreateUnique<PrefabEditorPanel>(id, m_Context, m_Assets, m_ImGui, m_Types,
                                                       m_Editors, m_Sources, m_Input, m_Systems);
            }

        private:
            Renderer::Context& m_Context;
            AssetManager& m_Assets;
            ImGuiLayer& m_ImGui;
            TypeRegistry& m_Types;
            EditorRegistry& m_Editors;
            const AssetSourceIndex& m_Sources;
            Input& m_Input;
            SystemRegistry& m_Systems;
        };
    }

    // Owns the host-side registries so the base Application can borrow the TypeRegistry.
    struct EditorHost::Registries
    {
        ApplicationRegistry App;
        TypeRegistry Types;
        SystemRegistry Systems;
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
            .Systems = registries->Systems,
            .Editor = &registries->Editor,
        };
        gameModule->Register(host);
        if (editorModule)
        {
            editorModule->Register(host);
        }

        auto gameModulePtr = CreateUnique<LoadedModule>(std::move(*gameModule));
        return Unique<EditorHost>(new EditorHost(
            info, std::move(registries), std::move(gameModulePtr), std::move(editorModule)));
    }

    EditorHost::EditorHost(const EditorHostInfo& info, Unique<Registries> registries,
                           Unique<LoadedModule> gameModule, optional<LoadedModule> editorModule)
        : Application(info.App, registries->Types, registries->Systems), m_Info(info),
          m_Registries(std::move(registries)), m_GameModule(std::move(gameModule)),
          m_EditorModule(std::move(editorModule))
    {
    }

    EditorHost::~EditorHost() = default;

    void EditorHost::OpenAssetEditor(AssetType type, AssetId id)
    {
        if (Unique<EditorPanel> panel = m_Registries->Editor.CreateEditorFor(type, id))
        {
            m_PendingPanels.push_back(std::move(panel));
        }
    }

    bool EditorHost::HasAssetEditor(AssetType type) const
    {
        return m_Registries->Editor.AssetEditorFor(type) != nullptr;
    }

    void EditorHost::OnInitialize()
    {
        VE_ASSERT(GetImGuiLayer() != nullptr, "editor host requires the ImGui layer");

        const VoidResult mount = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mount, "{}", mount.error());

        {
            const AssetResult<AssetHandle<Shader>> vs =
                GetAssetManager().LoadSync<Shader>(FullscreenVertId);
            VE_ASSERT(vs.has_value(), "{}", vs.error().Detail);
            const AssetResult<AssetHandle<Shader>> fs =
                GetAssetManager().LoadSync<Shader>(BlitFragId);
            VE_ASSERT(fs.has_value(), "{}", fs.error().Detail);
            m_BlitVS = *vs;
            m_BlitFS = *fs;

            m_BlitLayout = Renderer::PipelineLayout::Create(
                GetRenderContext(), {
                                        .Name = "Editor Blit Layout",
                                        .PushConstantRanges =
                                            {
                                                Renderer::PushConstantRange::Of<BlitPushConstants>(
                                                    Renderer::ShaderStage::Fragment),
                                            },
                                    });

            m_BlitPipeline = Renderer::GraphicsPipeline::Create(
                GetRenderContext(),
                {
                    .Name = "Editor Blit Pipeline",
                    .ColorAttachments = {{.Format = GetRenderContext().GetSwapChainFormat()}},
                    .PipelineLayout = m_BlitLayout,
                    .ShaderStages =
                        {
                            {.Stage = Renderer::ShaderStage::Vertex,
                             .Module = m_BlitVS.Get()->Module},
                            {.Stage = Renderer::ShaderStage::Fragment,
                             .Module = m_BlitFS.Get()->Module},
                        },
                });
        }

        m_Sampler = Renderer::Sampler::Create(
            GetRenderContext(), {
                                    .Name = "Editor Present Sampler",
                                    .AddressModeU = Renderer::AddressMode::ClampToEdge,
                                    .AddressModeV = Renderer::AddressMode::ClampToEdge,
                                    .AddressModeW = Renderer::AddressMode::ClampToEdge,
                                });

        m_ImGuiView = Renderer::ImageView::Create(GetRenderContext(),
                                                  {
                                                      .Name = "Editor ImGui View",
                                                      .Image = GetImGuiLayer()->GetOutputImage(),
                                                  });

        auto& bindless = GetRenderContext().GetBindlessRegistry();
        m_ImGuiHandle = bindless.Register(m_ImGuiView);
        m_SamplerHandle = bindless.Register(m_Sampler);

        m_PresentGraph = BuildPresentGraph();
        GetRenderContext().AddSwapChainInvalidationCallback(
            [this] { m_PresentGraph = BuildPresentGraph(); });

        // Parsed once; an empty index when no manifest is configured keeps the picker
        // candidate-free rather than absent.
        m_Sources = CreateUnique<AssetSourceIndex>(
            m_Info.AssetManifestPath ? AssetSourceIndex::Parse(*m_Info.AssetManifestPath)
                                     : AssetSourceIndex{});

        // A prefab is edited live in a spawned Scene, so its editor needs no manifest
        // source; register it unconditionally.
        m_Registries->Editor.RegisterAssetEditor(
            AssetType::Prefab,
            CreateUnique<PrefabEditorFactory>(
                GetRenderContext(), GetAssetManager(), *GetImGuiLayer(), GetTypeRegistry(),
                m_Registries->Editor, *m_Sources, GetInput(), GetSystemRegistry()));

        // try_emplace no-ops if the game module already registered a factory for these types.
        if (m_Info.AssetManifestPath)
        {
            auto cookFor = [this]
            {
                return VengEditor::CookDriver([this](const VengEditor::CookRequest& request,
                                                     function<void(Result<MountHandle>)> onComplete)
                                              { RequestCook(request, std::move(onComplete)); });
            };

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Texture,
                CreateUnique<TextureEditorFactory>(*m_Sources, GetRenderContext(),
                                                   GetAssetManager(), *GetImGuiLayer(), cookFor()));

            m_Registries->Editor.RegisterAssetEditor(
                AssetType::Material, CreateUnique<MaterialEditorFactory>(
                                         *m_Sources, GetRenderContext(), GetAssetManager(),
                                         *GetImGuiLayer(), m_Registries->Editor, cookFor()));
        }

        m_Panels.push_back({CreateUnique<AssetBrowserPanel>(
                                ExecutableDirectory() / "sample.vengpack", *m_Sources, *this),
                            true});
        m_Panels.push_back({CreateUnique<ConsolePanel>(), true});

        for (Unique<EditorPanel>& panel : m_Registries->Editor.Panels())
        {
            m_Panels.push_back({std::move(panel), true});
        }

        // Open the sample prefab as the initial document so the editor starts on live
        // content; double-clicking any prefab in the asset browser opens another.
        OpenAssetEditor(AssetType::Prefab, SampleScenePrefabId);
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
                .Clear = Renderer::ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(m_ImGuiId)
            .Execute(
                [this](Renderer::PassContext& ctx)
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

        // Inject the manifest path so the cook resolves cross-asset references by AssetId;
        // panels build manifest-agnostic requests.
        CookRequest resolved = request;
        if (m_Info.AssetManifestPath)
        {
            resolved.ReferenceManifest = *m_Info.AssetManifestPath;
        }

        Task<vector<u8>> task = m_Info.Cook(resolved, GetTaskSystem());

        task.Then(
            [this, targetId = request.TargetId, source = request.SourcePath,
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
        if (auto bar = UI::MainMenuBar())
        {
            if (auto file = UI::Menu("File"))
            {
                if (UI::MenuItem("Exit"))
                {
                    RequestExit();
                }
            }

            if (auto window = UI::Menu("Window"))
            {
                for (PanelSlot& slot : m_Panels)
                {
                    UI::MenuItem(slot.Panel->GetTitle(), &slot.Open);
                }
            }
        }
    }

    void EditorHost::OnRender()
    {
        auto& cmd = GetRenderContext().GetCurrentCommandBuffer();

        // Adopt any panels opened via OpenAssetEditor since last frame, before the
        // render/draw passes so a freshly opened editor renders this frame.
        for (Unique<EditorPanel>& opened : m_PendingPanels)
        {
            m_Panels.push_back({std::move(opened), true});
        }
        m_PendingPanels.clear();

        // Offscreen render pass: a render-owning panel (e.g. a prefab viewport) records
        // its scene render here so the output is sampleable when its window draws it.
        for (PanelSlot& slot : m_Panels)
        {
            if (slot.Open)
            {
                slot.Panel->OnRender(cmd);
            }
        }

        ImGui::DockSpaceOverViewport();
        DrawMenuBar();

        // Each panel submits its own top-level window(s); an asset editor submits its
        // private dockspace and the children docked into it.
        for (PanelSlot& slot : m_Panels)
        {
            if (slot.Open)
            {
                slot.Panel->Draw(&slot.Open);
            }
        }

        GetImGuiLayer()->Render(cmd);

        const Renderer::RenderGraph::ImportBinding bindings[] = {
            {.Id = m_SwapId, .View = GetRenderContext().GetCurrentSwapChainImageView()},
            {.Id = m_ImGuiId, .View = m_ImGuiView},
        };
        m_PresentGraph->Execute(cmd, bindings);
    }

    void EditorHost::OnDispose()
    {
        m_Panels.clear();
        m_PendingPanels.clear();
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
