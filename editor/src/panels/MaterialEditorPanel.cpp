#include "MaterialEditorPanel.h"

#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "FieldWidget.h"

#include <VengGraph/MaterialCompile.h>

#include <Veng/Application.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

// imnodes drives the node canvas (vec2 crosses into its positioning calls via the
// glm<->ImVec2 conversion).
#include <Veng/Vendor/ImGui.h>

#include <VengGraph/NodeGraphSerialize.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <sstream>

namespace VengEditor
{
    using namespace Veng;
    using namespace VengGraph;
    using Json = nlohmann::json;

    namespace
    {
        constexpr f32 DebounceSeconds = 0.3f;
        constexpr f32 ToastSeconds = 3.0f;
        constexpr uvec2 PreviewExtent{256, 256};

        // The serialized "_editor" block carries the same NodeGraph document the
        // serializer writes, under this key, in the .vmat.json.
        constexpr const char* EditorKey = "_editor";

        // imnodes ids: a live NodeId's Index is unique among live nodes, so it is
        // the node id; attribute ids fold the node index, the input/output bit, and
        // the pin slot into one int. Node ids are offset by 1 so 0 is never used.
        constexpr int NodeIdShift = 12;
        constexpr int OutputBit = 1 << 11;

        int NodeImId(NodeId node)
        {
            return static_cast<int>(node.Index) + 1;
        }

        int AttrId(NodeId node, u16 pin, bool isOutput)
        {
            return (NodeImId(node) << NodeIdShift) | (isOutput ? OutputBit : 0) |
                   static_cast<int>(pin);
        }

        u16 PinOf(int attr)
        {
            return static_cast<u16>(attr & (OutputBit - 1));
        }
        bool IsOutputAttr(int attr)
        {
            return (attr & OutputBit) != 0;
        }
        u32 NodeIndexOf(int attr)
        {
            return static_cast<u32>((attr >> NodeIdShift) - 1);
        }
    }

    MaterialEditorPanel::MaterialEditorPanel(AssetId id, path sourcePath,
                                             const AssetSourceIndex& sources, Application& app,
                                             AssetManager& assets, ImGuiLayer& imgui,
                                             EditorRegistry& editors, CookDriver cook)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Sources(sources),
          m_Context(app.GetRenderContext()), m_Assets(assets), m_ImGui(imgui), m_Editors(editors),
          m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Material: {}", m_SourcePath.filename().string());

        // The temp cook source is a fixed dotfile beside the real source so the
        // importer's source-dir-relative .shader.json resolution still resolves.
        m_TempPath = m_SourcePath.parent_path() /
                     fmt::format(".{}.editor-tmp.vmat.json", m_SourcePath.stem().string());

        m_NodeEditorContext = ImNodes::EditorContextCreate();

        // The preview owns an Offscreen viewport; register it so the engine drive-list renders it.
        m_Preview = CreateUnique<MaterialPreview>(m_Context, m_Assets, m_ImGui, PreviewExtent);
        app.RegisterViewport(m_Preview->GetViewport());

        if (!LoadInterface())
        {
            return;
        }

        ResolveShaderSource();
        BuildGraph();
        TriggerCook();
    }

    MaterialEditorPanel::~MaterialEditorPanel()
    {
        m_Preview.reset();
        ImNodes::EditorContextFree(m_NodeEditorContext);

        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    AssetResult<AssetHandle<MaterialInstance>> MaterialEditorPanel::PreviewInstance()
    {
        // The previewed material is a runtime zero-override instance over the edited parent, so the
        // preview shows the parent's authored defaults and GetFields()/GetDomain() delegate to its
        // schema. The parent has no cooked default-instance id of its own here (the editor cooks
        // only the parent), so the default instance is built at runtime rather than loaded by id.
        const AssetResult<AssetHandle<Material>> parent = m_Assets.LoadSync<Material>(m_Id);
        if (!parent)
        {
            return std::unexpected(parent.error());
        }

        return m_Assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
            .Name = fmt::format("Material editor preview {}", m_Id.Value),
            .Context = &m_Context,
            .Parent = *parent,
            .Overrides = {},
        });
    }

    bool MaterialEditorPanel::LoadInterface()
    {
        const AssetResult<AssetHandle<MaterialInstance>> loaded = PreviewInstance();
        if (!loaded)
        {
            m_CookError = loaded.error().Detail;
            Log::Error("Material editor: failed to load 0x{:X}: {}", m_Id.Value,
                       loaded.error().Detail);
            return false;
        }

        const MaterialInstance& material = *loaded->Get();
        m_Fields.assign(material.GetFields().begin(), material.GetFields().end());
        m_Domain = material.GetDomain();

        // The cooked Material does not surface its shader ids; read them from the
        // source document. They round-trip through the regenerated "shaders" block.
        const std::ifstream file(m_SourcePath, std::ios::binary);
        if (file)
        {
            std::ostringstream contents;
            contents << file.rdbuf();
            const Json doc = Json::parse(contents.str(), nullptr, false);
            if (!doc.is_discarded() && doc.is_object() && doc.contains("shaders") &&
                doc["shaders"].is_object())
            {
                const Json& shaders = doc["shaders"];
                if (shaders.contains("vertex") && shaders["vertex"].is_number_unsigned())
                {
                    m_VertexShader = AssetId{shaders["vertex"].get<u64>()};
                }
                if (shaders.contains("fragment") && shaders["fragment"].is_number_unsigned())
                {
                    m_FragmentShader = AssetId{shaders["fragment"].get<u64>()};
                }
            }
        }

        return true;
    }

    void MaterialEditorPanel::ResolveShaderSource()
    {
        // A graph-sourced fragment shader names a *.graph.json in its *.shader.json; the panel
        // then edits that graph (the shader's source of truth) and the cook generates Slang
        // from it. A *.slang source leaves m_GraphSourced false.
        const AssetSourceIndex::Entry* entry = m_Sources.Find(m_FragmentShader);
        if (entry == nullptr)
        {
            return;
        }

        const std::ifstream file(entry->Source, std::ios::binary);
        if (!file)
        {
            return;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        const Json doc = Json::parse(contents.str(), nullptr, false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("source") ||
            !doc["source"].is_string())
        {
            return;
        }

        const string source = doc["source"].get<string>();
        constexpr std::string_view GraphSuffix = ".graph.json";
        if (source.size() < GraphSuffix.size() ||
            source.compare(source.size() - GraphSuffix.size(), GraphSuffix.size(), GraphSuffix) !=
                0)
        {
            return;
        }

        m_GraphSourced = true;
        m_ShaderJsonPath = entry->Source;
        m_GraphPath = entry->Source.parent_path() / source;
        m_FragmentEntry = (doc.contains("entry") && doc["entry"].is_string())
                              ? doc["entry"].get<string>()
                              : string("fsMain");
    }

    MaterialShaderInterface MaterialEditorPanel::Interface() const
    {
        return MaterialShaderInterface{
            .Fields = m_Fields,
            .VertexShader = m_VertexShader,
            .FragmentShader = m_FragmentShader,
        };
    }

    void MaterialEditorPanel::BuildGraph()
    {
        m_Types = RegisterMaterialNodeTypes(m_Catalog, m_Emit, m_Domain);

        // The authored graph lives in the shader's own *.graph.json when the fragment shader
        // is graph-sourced (the codegen path); otherwise it is embedded under "_editor" in the
        // .vmat. Read whichever is the source of truth; a newer version → read-only, absent →
        // a default graph (a bare MaterialOutput).
        Json editorBlock;
        bool haveBlock = false;
        if (m_GraphSourced)
        {
            const std::ifstream file(m_GraphPath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const Json doc = Json::parse(contents.str(), nullptr, false);
                if (!doc.is_discarded() && doc.is_object())
                {
                    editorBlock = doc;
                    haveBlock = true;
                }
            }
        }
        else
        {
            const std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const Json doc = Json::parse(contents.str(), nullptr, false);
                if (!doc.is_discarded() && doc.is_object() && doc.contains(EditorKey) &&
                    doc[EditorKey].is_object())
                {
                    editorBlock = doc[EditorKey];
                    haveBlock = true;
                }
            }
        }

        if (haveBlock)
        {
            m_Graph = CreateUnique<NodeGraph>(
                MaterialCanConnect, [this](NodeTypeId nid) { return m_Catalog.ShapeOf(nid); },
                [this](NodeTypeId nid)
                {
                    const NodeType* type = m_Catalog.Find(nid);
                    return type != nullptr ? type->PropertySize : usize{0};
                });

            const NodeGraphReadOutcome outcome =
                ReadNodeGraph(editorBlock.dump(), *m_Graph, m_Catalog);
            if (outcome == NodeGraphReadOutcome::VersionTooNew)
            {
                m_ReadOnly = true;
                Log::Warn("Material editor: '{}' has a newer graph version; opening read-only",
                          m_SourcePath.filename().string());
            }
            else if (outcome == NodeGraphReadOutcome::Malformed)
            {
                // A malformed block falls back to the default graph.
                m_Graph = CreateUnique<NodeGraph>(MakeDefaultGraph());
            }
        }
        else
        {
            m_Graph = CreateUnique<NodeGraph>(MakeDefaultGraph());
        }
    }

    NodeGraph MaterialEditorPanel::MakeDefaultGraph() const
    {
        NodeGraph graph(
            MaterialCanConnect, [this](NodeTypeId nid) { return m_Catalog.ShapeOf(nid); },
            [this](NodeTypeId nid)
            {
                const NodeType* type = m_Catalog.Find(nid);
                return type != nullptr ? type->PropertySize : usize{0};
            });
        const NodeId output = graph.AddNode(m_Types.MaterialOutput);
        graph.MoveNode(output, vec2{600.0f, 0.0f});
        return graph;
    }

    void MaterialEditorPanel::MarkDirty()
    {
        if (m_ReadOnly)
        {
            return;
        }
        m_CookPending = true;
        m_DebounceRemaining = DebounceSeconds;
    }

    optional<string> MaterialEditorPanel::AssembleVmat() const
    {
        // A graph-sourced material's field list is generated from the same walk that
        // generates the shader: the texture nodes' AssetIds + the exposed params' defaults,
        // so the .vmat's packed values agree with the shader's reflected offsets by
        // construction. The graph itself lives in the shader's .graph.json, not the .vmat, so
        // no "_editor" block is embedded.
        if (m_GraphSourced)
        {
            const Result<GeneratedFragment> generated =
                CompileMaterialGraph(*m_Graph, m_Catalog, m_Emit, m_Domain);
            if (!generated)
            {
                return std::nullopt;
            }
            return WriteMaterialVmat(generated->Fields, Interface(), m_Domain);
        }

        // The non-graph material stays on its on-disk field list; only the embedded "_editor"
        // graph block is regenerated. Reading the source preserves "domain"/"shaders"/"fields"
        // and every unknown key.
        Json doc = Json::object();
        {
            const std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const Json parsed = Json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                {
                    doc = parsed;
                }
            }
        }

        // Parse the serializer's string into a JSON object so "_editor" embeds as
        // a nested object rather than an escaped string.
        const Json graphDoc = Json::parse(WriteNodeGraph(*m_Graph, m_Catalog), nullptr, false);
        if (!graphDoc.is_discarded())
        {
            doc[EditorKey] = graphDoc;
        }

        return doc.dump(4);
    }

    bool MaterialEditorPanel::WriteVmat(const path& target)
    {
        const optional<string> document = AssembleVmat();
        if (!document)
        {
            m_CookError = "compile failed; not written";
            Log::Error("Material editor: compile failed, not writing {}", target.string());
            return false;
        }

        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            m_CookError = fmt::format("failed to write {}", target.string());
            Log::Error("Material editor: {}", *m_CookError);
            return false;
        }
        out << *document << '\n';
        return true;
    }

    void MaterialEditorPanel::TriggerCook()
    {
        if (m_Cooking || m_Graph == nullptr)
        {
            return;
        }

        // Write the temp source next to the real one (source-dir-relative resolves);
        // a compile failure aborts the cook with an inline error.
        if (!WriteVmat(m_TempPath))
        {
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        // A graph-sourced shader is cooked first (the generated SPIR-V the material binds),
        // then the material; both mounts are held. The graph is the shader's source of truth,
        // so the edit is persisted to its .graph.json and the material cook reflects the
        // regenerated MaterialParams from it by id-resolution. A non-graph material cooks alone.
        if (!m_GraphSourced)
        {
            CookMaterial();
            return;
        }

        // Persist the edited graph to the shader's .graph.json, then cook the shader.
        {
            std::ofstream out(m_GraphPath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                m_Cooking = false;
                m_CookError = fmt::format("failed to write {}", m_GraphPath.string());
                Log::Error("Material editor: {}", *m_CookError);
                return;
            }
            out << WriteNodeGraph(*m_Graph, m_Catalog) << '\n';
        }

        m_Cook({.SourcePath = m_ShaderJsonPath,
                .TargetId = m_FragmentShader,
                .Type = AssetType::Shader},
               [this](Result<MountHandle> mount)
               {
                   if (!mount)
                   {
                       m_Cooking = false;
                       m_CookError = mount.error();
                       Log::Error("Material editor: shader cook failed: {}", mount.error());
                       return;
                   }
                   m_ShaderMount = std::move(*mount);
                   CookMaterial();
               });
    }

    void MaterialEditorPanel::CookMaterial()
    {
        m_Cooking = true;
        m_Cook({.SourcePath = m_TempPath, .TargetId = m_Id, .Type = AssetType::Material},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (!mount)
                   {
                       m_CookError = mount.error();
                       Log::Error("Material editor: cook failed: {}", mount.error());
                       return;
                   }

                   // Replace the mount and rebuild a default instance over the freshly-cooked
                   // parent; OnUI swaps the handle into the preview once it is resident.
                   m_Mount = std::move(*mount);
                   AssetResult<AssetHandle<MaterialInstance>> rebuilt = PreviewInstance();
                   if (!rebuilt)
                   {
                       m_CookError = rebuilt.error().Detail;
                       Log::Error("Material editor: preview rebuild failed: {}",
                                  rebuilt.error().Detail);
                       return;
                   }
                   m_Handle = std::move(*rebuilt);
                   m_MaterialDirty = true;
               });
    }

    bool MaterialEditorPanel::DrawCanvas()
    {
        bool mutated = false;

        ImNodes::EditorContextSet(m_NodeEditorContext);

        // The add-node context menu: lists every catalog type.
        if (auto menu = UI::Popup("AddNodeMenu"))
        {
            const vec2 mouse = UI::PopupMousePosition();
            for (const NodeType& type : m_Catalog.Types())
            {
                if (UI::MenuItem(type.Name))
                {
                    const NodeId node = m_Graph->AddNode(type.Id);
                    ImNodes::SetNodeScreenSpacePos(NodeImId(node), mouse);
                    mutated = true;
                }
            }
        }

        ImNodes::BeginNodeEditor();

        for (const NodeId node : m_Graph->Nodes())
        {
            const NodeType* type = m_Catalog.Find(m_Graph->GetTypeOf(node));
            if (type == nullptr)
            {
                continue;
            }

            ImNodes::BeginNode(NodeImId(node));

            ImNodes::BeginNodeTitleBar();
            UI::Text(type->Name);
            ImNodes::EndNodeTitleBar();

            for (usize i = 0; i < type->Inputs.size(); ++i)
            {
                ImNodes::BeginInputAttribute(AttrId(node, static_cast<u16>(i), false));
                UI::Text(type->Inputs[i].Name);
                ImNodes::EndInputAttribute();
            }
            for (usize i = 0; i < type->Outputs.size(); ++i)
            {
                ImNodes::BeginOutputAttribute(AttrId(node, static_cast<u16>(i), true));
                UI::Text(type->Outputs[i].Name);
                ImNodes::EndOutputAttribute();
            }

            ImNodes::EndNode();
        }

        // Links: imnodes link id is the link's position in the live link array.
        const std::span<const Link> links = m_Graph->Links();
        for (usize i = 0; i < links.size(); ++i)
        {
            const Link& link = links[i];
            ImNodes::Link(static_cast<int>(i), AttrId(link.From.Node, link.From.Pin, true),
                          AttrId(link.To.Node, link.To.Pin, false));
        }

        ImNodes::EndNodeEditor();

        // Right-click the canvas → add-node menu.
        if (ImNodes::IsEditorHovered() && UI::IsMouseClicked(UI::MouseButton::Right))
        {
            UI::OpenPopup("AddNodeMenu");
        }

        // Decode a NodeId from a node imnodes id by scanning the live set (the
        // index is unique among live nodes).
        const auto nodeFromIndex = [&](u32 index) -> NodeId
        {
            for (NodeId n : m_Graph->Nodes())
            {
                if (n.Index == index)
                {
                    return n;
                }
            }
            return NodeId{};
        };

        if (!m_ReadOnly)
        {
            int startAttr = 0;
            int endAttr = 0;
            if (ImNodes::IsLinkCreated(&startAttr, &endAttr))
            {
                // The start pin is the output, the end pin the input; imnodes does
                // not guarantee which is which, so disambiguate by the output bit.
                const int outAttr = IsOutputAttr(startAttr) ? startAttr : endAttr;
                const int inAttr = IsOutputAttr(startAttr) ? endAttr : startAttr;
                if (IsOutputAttr(outAttr) && !IsOutputAttr(inAttr))
                {
                    const PinRef from{.Node = nodeFromIndex(NodeIndexOf(outAttr)),
                                      .Pin = PinOf(outAttr)};
                    const PinRef to{.Node = nodeFromIndex(NodeIndexOf(inAttr)),
                                    .Pin = PinOf(inAttr)};
                    const VoidResult result = m_Graph->Connect(from, to);
                    if (!result)
                    {
                        m_Toast = result.error();
                        m_ToastRemaining = ToastSeconds;
                    }
                    else
                    {
                        mutated = true;
                    }
                }
            }

            int destroyed = 0;
            if (ImNodes::IsLinkDestroyed(&destroyed))
            {
                const std::span<const Link> live = m_Graph->Links();
                if (destroyed >= 0 && static_cast<usize>(destroyed) < live.size())
                {
                    m_Graph->Disconnect(live[destroyed]);
                    mutated = true;
                }
            }

            // Delete key removes the selected nodes.
            if (UI::IsKeyPressed(UI::Key::Delete) || UI::IsKeyPressed(UI::Key::Backspace))
            {
                const int count = ImNodes::NumSelectedNodes();
                if (count > 0)
                {
                    vector<int> selected(static_cast<usize>(count));
                    ImNodes::GetSelectedNodes(selected.data());
                    for (const int imId : selected)
                    {
                        const NodeId node = nodeFromIndex(static_cast<u32>(imId - 1));
                        if (m_Graph->IsValid(node))
                        {
                            m_Graph->RemoveNode(node);
                        }
                    }
                    ImNodes::ClearNodeSelection();
                    mutated = true;
                }
            }
        }

        // Persist node drags back into the model so a recook/save records them.
        for (const NodeId node : m_Graph->Nodes())
        {
            const vec2 pos = ImNodes::GetNodeGridSpacePos(NodeImId(node));
            if (pos != m_Graph->PositionOf(node))
            {
                m_Graph->MoveNode(node, pos);
            }
        }

        return mutated;
    }

    bool MaterialEditorPanel::DrawNodeInspector()
    {
        const int count = ImNodes::NumSelectedNodes();
        if (count <= 0)
        {
            UI::TextDisabled("Select a node");
            return false;
        }

        vector<int> selected(static_cast<usize>(count));
        ImNodes::GetSelectedNodes(selected.data());

        // Decode the first selected node.
        NodeId node{};
        for (const NodeId n : m_Graph->Nodes())
        {
            if (n.Index == static_cast<u32>(selected[0] - 1))
            {
                node = n;
            }
        }
        if (!m_Graph->IsValid(node))
        {
            UI::TextDisabled("Select a node");
            return false;
        }

        const NodeType* type = m_Catalog.Find(m_Graph->GetTypeOf(node));
        if (type == nullptr || type->Properties.empty())
        {
            UI::TextDisabled("(no properties)");
            return false;
        }

        UI::Text(type->Name);
        UI::Separator();

        // Draw widgets over a scratch copy; route changes back through the mutation
        // vocabulary so the graph stays the single writable path.
        const std::span<const std::byte> bytes = m_Graph->PropertyBytes(node);
        vector<std::byte> scratch(bytes.begin(), bytes.end());

        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};

        {
            auto disabled = UI::Disabled(m_ReadOnly);
            if (auto table = UI::PropertyTable("##nodeprops"))
            {
                (void)DrawFields(scratch.data(), type->Properties, ctx);
            }
        }

        if (m_ReadOnly || scratch.size() != bytes.size() ||
            std::memcmp(scratch.data(), bytes.data(), bytes.size()) == 0)
        {
            return false;
        }

        for (usize i = 0; i < type->Properties.size(); ++i)
        {
            const FieldDescriptor& field = type->Properties[i];
            const usize end =
                (i + 1 < type->Properties.size()) ? type->Properties[i + 1].Offset : scratch.size();
            const usize size = end - field.Offset;
            m_Graph->SetProperty(node, field,
                                 std::span<const std::byte>(scratch.data() + field.Offset, size));
        }
        return true;
    }

    void MaterialEditorPanel::OnUI()
    {
        // Debounce so a slider drag does not fire a cook per frame.
        if (m_CookPending)
        {
            m_DebounceRemaining -= Time::GetDeltaTime();
            if (m_DebounceRemaining <= 0.0f)
            {
                m_CookPending = false;
                TriggerCook();
            }
        }

        if (m_ToastRemaining > 0.0f)
        {
            m_ToastRemaining -= Time::GetDeltaTime();
        }

        // Swap the freshly loaded material into the preview once resident.
        if (m_MaterialDirty && m_Handle.IsLoaded())
        {
            m_Preview->SetMaterial(m_Handle);
            m_MaterialDirty = false;
            m_PreviewReady = true;
        }

        // Advance the turntable and push this frame's view; the engine renders the preview's
        // registered viewport at the next frame's start.
        m_Preview->Update();

        if (m_Graph == nullptr)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Material failed to load");
            if (m_CookError)
            {
                UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, *m_CookError);
            }
            return;
        }

        // Toolbar.
        {
            auto disabled = UI::Disabled(m_ReadOnly);
            if (UI::Button(Icons::Save))
            {
                // A graph-sourced material's authored graph is the shader's .graph.json;
                // persist it beside the regenerated .vmat field list.
                if (m_GraphSourced)
                {
                    std::ofstream out(m_GraphPath, std::ios::binary | std::ios::trunc);
                    if (out)
                    {
                        out << WriteNodeGraph(*m_Graph, m_Catalog) << '\n';
                    }
                }
                WriteVmat(m_SourcePath);
            }
            UI::Tooltip("Save the material graph to its .vmat.json");
        }
        UI::SameLine();
        if (UI::Button(Icons::Revert))
        {
            m_Catalog = NodeCatalog{};
            m_Emit = MaterialEmitTable{};
            m_ReadOnly = false;
            BuildGraph();
            TriggerCook();
        }
        UI::Tooltip("Discard edits and reload the material from disk");
        if (m_ReadOnly)
        {
            UI::SameLine();
            UI::TextColored({0.9f, 0.8f, 0.3f, 1.0f}, "(read-only: newer graph version)");
        }
        if (m_Cooking)
        {
            UI::SameLine();
            UI::Text("Cooking...");
        }
        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }
        if (m_Toast && m_ToastRemaining > 0.0f)
        {
            UI::TextColored({0.9f, 0.6f, 0.3f, 1.0f}, fmt::format("Rejected: {}", *m_Toast));
        }

        UI::Separator();

        // Layout: a preview + node inspector column on the left, the canvas on the
        // right.
        const f32 sideWidth = 280.0f;
        if (auto side = UI::Child("MatSide", vec2(sideWidth, 0)))
        {
            const f32 previewSide = PreviewExtent.x;
            if (m_PreviewReady)
            {
                UI::Image(m_Preview->GetTexture(), vec2(previewSide, previewSide));
            }
            else
            {
                UI::Text("Preview loading...");
            }
            UI::Separator();
            DrawNodeInspector();
        }

        UI::SameLine();

        bool mutated = false;
        if (auto canvas = UI::Child("MatCanvas"))
        {
            mutated = DrawCanvas();
        }

        if (mutated)
        {
            MarkDirty();
        }
    }
}
