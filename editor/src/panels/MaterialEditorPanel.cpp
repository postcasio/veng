#include "MaterialEditorPanel.h"

#include "AssetSourceIndex.h"
#include "FieldWidget.h"

#include "material/MaterialCompile.h"

#include <Veng/Asset/Material.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Vendor/ImGui.h>

#include <VengEditor/NodeGraph/NodeGraphSerialize.h>

#include <imnodes.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <sstream>

namespace VengEditor
{
    using namespace Veng;
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

        int NodeImId(NodeId node) { return static_cast<int>(node.Index) + 1; }

        int AttrId(NodeId node, u16 pin, bool isOutput)
        {
            return (NodeImId(node) << NodeIdShift) | (isOutput ? OutputBit : 0) | static_cast<int>(pin);
        }

        u16 PinOf(int attr) { return static_cast<u16>(attr & (OutputBit - 1)); }
        bool IsOutputAttr(int attr) { return (attr & OutputBit) != 0; }
        u32 NodeIndexOf(int attr) { return static_cast<u32>((attr >> NodeIdShift) - 1); }
    }

    MaterialEditorPanel::MaterialEditorPanel(AssetId id, path sourcePath,
                                             const AssetSourceIndex& sources, Renderer::Context& context,
                                             AssetManager& assets, ImGuiLayer& imgui,
                                             EditorRegistry& editors, CookDriver cook) :
        m_Id(id), m_SourcePath(std::move(sourcePath)), m_Sources(sources), m_Context(context),
        m_Assets(assets), m_ImGui(imgui), m_Editors(editors), m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Material: {}", m_SourcePath.filename().string());

        // The temp cook source is a fixed dotfile beside the real source so the
        // importer's source-dir-relative .shader.json resolution still resolves.
        m_TempPath = m_SourcePath.parent_path() /
                     fmt::format(".{}.editor-tmp.vmat.json", m_SourcePath.stem().string());

        // The library-singleton imnodes context is owned by libveng's ImGuiLayer.
        // A panel owns only its own canvas state, so multiple material editors get
        // isolated panning/node positions and tearing one down never touches the
        // shared singleton.
        m_NodeEditorContext = ImNodes::EditorContextCreate();

        m_Preview = CreateUnique<MaterialPreview>(m_Context, m_Assets, m_ImGui, PreviewExtent);

        if (!LoadInterface())
            return;

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

    bool MaterialEditorPanel::LoadInterface()
    {
        const AssetResult<AssetHandle<Material>> loaded = m_Assets.LoadSync<Material>(m_Id);
        if (!loaded)
        {
            m_CookError = loaded.error().Detail;
            Log::Error("Material editor: failed to load 0x{:X}: {}", m_Id.Value, loaded.error().Detail);
            return false;
        }

        const Material& material = *loaded->Get();
        m_Fields.assign(material.GetFields().begin(), material.GetFields().end());

        // The cooked Material does not surface its shader ids; read them from the
        // source document. They round-trip through the regenerated "shaders" block.
        std::ifstream file(m_SourcePath, std::ios::binary);
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
                    m_VertexShader = AssetId{shaders["vertex"].get<u64>()};
                if (shaders.contains("fragment") && shaders["fragment"].is_number_unsigned())
                    m_FragmentShader = AssetId{shaders["fragment"].get<u64>()};
            }
        }

        return true;
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
        const MaterialShaderInterface iface = Interface();
        m_Types = RegisterMaterialNodeTypes(m_Catalog, iface);

        // Read the "_editor" block if present; a newer version → read-only, no graph
        // regeneration. Absent → synthesize a default graph from the field table.
        Json editorBlock;
        bool haveBlock = false;
        {
            std::ifstream file(m_SourcePath, std::ios::binary);
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
                MaterialCanConnect,
                [this](NodeTypeId nid) { return m_Catalog.ShapeOf(nid); },
                [this](NodeTypeId nid) {
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
                // A malformed block falls back to a synthesized graph.
                m_Graph = CreateUnique<NodeGraph>(BuildGraphFromMaterial(iface, m_Catalog, m_Types));
            }
        }
        else
        {
            m_Graph = CreateUnique<NodeGraph>(BuildGraphFromMaterial(iface, m_Catalog, m_Types));
        }
    }

    void MaterialEditorPanel::MarkDirty()
    {
        if (m_ReadOnly)
            return;
        m_CookPending = true;
        m_DebounceRemaining = DebounceSeconds;
    }

    optional<string> MaterialEditorPanel::AssembleVmat() const
    {
        const MaterialShaderInterface iface = Interface();

        const Result<vector<CompiledField>> compiled =
            CompileMaterialGraph(*m_Graph, m_Catalog, iface);
        if (!compiled)
            return std::nullopt;

        // Read the existing source so unknown keys survive; patch "shaders",
        // "fields", and "_editor".
        Json doc = Json::object();
        {
            std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const Json parsed = Json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                    doc = parsed;
            }
        }

        // The compiled .vmat document carries the regenerated "shaders" + "fields";
        // merge those over the preserved base.
        const Json regenerated = Json::parse(WriteMaterialVmat(*compiled, iface), nullptr, false);
        if (!regenerated.is_discarded() && regenerated.is_object())
        {
            doc["shaders"] = regenerated["shaders"];
            doc["fields"] = regenerated["fields"];
        }

        // The graph document under "_editor" (a JSON object, parsed from the
        // serializer's string so the .vmat is a single document).
        const Json graphDoc = Json::parse(WriteNodeGraph(*m_Graph, m_Catalog), nullptr, false);
        if (!graphDoc.is_discarded())
            doc[EditorKey] = graphDoc;

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
            return;

        // Write the temp source next to the real one (source-dir-relative resolves);
        // a compile failure aborts the cook with an inline error.
        if (!WriteVmat(m_TempPath))
            return;

        m_Cooking = true;
        m_CookError.reset();

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

            // Replace the mount (old archive frees with the old handle's drop) and
            // re-fetch the material behind the stable id. The async load lands
            // resident on a later frame, where OnImGui swaps it into the preview.
            m_Mount = std::move(*mount);
            m_Handle = m_Assets.Load<Material>(m_Id);
            m_MaterialDirty = true;
        });
    }

    void MaterialEditorPanel::OnRender(Renderer::CommandBuffer& cmd)
    {
        if (m_Preview)
            m_Preview->Render(cmd);
    }

    bool MaterialEditorPanel::DrawCanvas()
    {
        bool mutated = false;

        ImNodes::EditorContextSet(m_NodeEditorContext);

        // The add-node context menu: lists every catalog type.
        if (ImGui::BeginPopup("AddNodeMenu"))
        {
            const ImVec2 mouse = ImGui::GetMousePosOnOpeningCurrentPopup();
            for (const NodeType& type : m_Catalog.Types())
            {
                if (ImGui::MenuItem(type.Name.c_str()))
                {
                    const NodeId node = m_Graph->AddNode(type.Id);
                    ImNodes::SetNodeScreenSpacePos(NodeImId(node), mouse);
                    mutated = true;
                }
            }
            ImGui::EndPopup();
        }

        ImNodes::BeginNodeEditor();

        for (NodeId node : m_Graph->Nodes())
        {
            const NodeType* type = m_Catalog.Find(m_Graph->GetTypeOf(node));
            if (type == nullptr)
                continue;

            ImNodes::BeginNode(NodeImId(node));

            ImNodes::BeginNodeTitleBar();
            ImGui::TextUnformatted(type->Name.c_str());
            ImNodes::EndNodeTitleBar();

            for (usize i = 0; i < type->Inputs.size(); ++i)
            {
                ImNodes::BeginInputAttribute(AttrId(node, static_cast<u16>(i), false));
                ImGui::TextUnformatted(type->Inputs[i].Name.c_str());
                ImNodes::EndInputAttribute();
            }
            for (usize i = 0; i < type->Outputs.size(); ++i)
            {
                ImNodes::BeginOutputAttribute(AttrId(node, static_cast<u16>(i), true));
                ImGui::TextUnformatted(type->Outputs[i].Name.c_str());
                ImNodes::EndOutputAttribute();
            }

            ImNodes::EndNode();
        }

        // Links: imnodes link id is the link's position in the live link array.
        const std::span<const Link> links = m_Graph->Links();
        for (usize i = 0; i < links.size(); ++i)
        {
            const Link& link = links[i];
            ImNodes::Link(static_cast<int>(i),
                          AttrId(link.From.Node, link.From.Pin, true),
                          AttrId(link.To.Node, link.To.Pin, false));
        }

        ImNodes::EndNodeEditor();

        // Right-click the canvas → add-node menu.
        if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("AddNodeMenu");

        // Decode a NodeId from a node imnodes id by scanning the live set (the
        // index is unique among live nodes).
        const auto nodeFromIndex = [&](u32 index) -> NodeId {
            for (NodeId n : m_Graph->Nodes())
                if (n.Index == index)
                    return n;
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
                    const PinRef from{nodeFromIndex(NodeIndexOf(outAttr)), PinOf(outAttr)};
                    const PinRef to{nodeFromIndex(NodeIndexOf(inAttr)), PinOf(inAttr)};
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
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
            {
                const int count = ImNodes::NumSelectedNodes();
                if (count > 0)
                {
                    vector<int> selected(static_cast<usize>(count));
                    ImNodes::GetSelectedNodes(selected.data());
                    for (int imId : selected)
                    {
                        const NodeId node = nodeFromIndex(static_cast<u32>(imId - 1));
                        if (m_Graph->IsValid(node))
                            m_Graph->RemoveNode(node);
                    }
                    ImNodes::ClearNodeSelection();
                    mutated = true;
                }
            }
        }

        // Persist node drags back into the model so a recook/save records them.
        for (NodeId node : m_Graph->Nodes())
        {
            const ImVec2 pos = ImNodes::GetNodeGridSpacePos(NodeImId(node));
            const vec2 current = m_Graph->PositionOf(node);
            if (pos.x != current.x || pos.y != current.y)
                m_Graph->MoveNode(node, vec2{pos.x, pos.y});
        }

        return mutated;
    }

    bool MaterialEditorPanel::DrawNodeInspector()
    {
        const int count = ImNodes::NumSelectedNodes();
        if (count <= 0)
        {
            ImGui::TextDisabled("Select a node");
            return false;
        }

        vector<int> selected(static_cast<usize>(count));
        ImNodes::GetSelectedNodes(selected.data());

        // Decode the first selected node.
        NodeId node{};
        for (NodeId n : m_Graph->Nodes())
            if (n.Index == static_cast<u32>(selected[0] - 1))
                node = n;
        if (!m_Graph->IsValid(node))
        {
            ImGui::TextDisabled("Select a node");
            return false;
        }

        const NodeType* type = m_Catalog.Find(m_Graph->GetTypeOf(node));
        if (type == nullptr || type->Properties.empty())
        {
            ImGui::TextDisabled("(no properties)");
            return false;
        }

        ImGui::TextUnformatted(type->Name.c_str());
        ImGui::Separator();

        // Copy the property bytes into a scratch buffer, draw the widgets over it,
        // then route any change back through the mutation vocabulary so the model
        // stays the single writable path. Each property's byte size is the span to
        // the next property's offset (the last runs to the buffer end).
        const std::span<const std::byte> bytes = m_Graph->PropertyBytes(node);
        vector<std::byte> scratch(bytes.begin(), bytes.end());

        const FieldWidgetContext ctx{.Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};

        ImGui::BeginDisabled(m_ReadOnly);
        for (const FieldDescriptor& field : type->Properties)
        {
            if (field.Hidden)
                continue;
            DrawFieldWidget(scratch.data() + field.Offset, field, ctx);
        }
        ImGui::EndDisabled();

        if (m_ReadOnly || scratch.size() != bytes.size() ||
            std::memcmp(scratch.data(), bytes.data(), bytes.size()) == 0)
            return false;

        for (usize i = 0; i < type->Properties.size(); ++i)
        {
            const FieldDescriptor& field = type->Properties[i];
            const usize end = (i + 1 < type->Properties.size())
                                  ? type->Properties[i + 1].Offset
                                  : scratch.size();
            const usize size = end - field.Offset;
            m_Graph->SetProperty(node, field,
                                 std::span<const std::byte>(scratch.data() + field.Offset, size));
        }
        return true;
    }

    void MaterialEditorPanel::OnImGui()
    {
        // Advance the debounce so a slider drag fires one settled cook.
        if (m_CookPending)
        {
            m_DebounceRemaining -= ImGui::GetIO().DeltaTime;
            if (m_DebounceRemaining <= 0.0f)
            {
                m_CookPending = false;
                TriggerCook();
            }
        }

        if (m_ToastRemaining > 0.0f)
            m_ToastRemaining -= ImGui::GetIO().DeltaTime;

        // Swap the freshly loaded material into the preview once resident.
        if (m_MaterialDirty && m_Handle.IsLoaded())
        {
            m_Preview->SetMaterial(m_Handle);
            m_MaterialDirty = false;
            m_PreviewReady = true;
        }

        if (m_Graph == nullptr)
        {
            ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Material failed to load");
            if (m_CookError)
                ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "%s", m_CookError->c_str());
            return;
        }

        // Toolbar.
        ImGui::BeginDisabled(m_ReadOnly);
        if (ImGui::Button("Save"))
            WriteVmat(m_SourcePath);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            m_Catalog = NodeCatalog{};
            m_ReadOnly = false;
            BuildGraph();
            TriggerCook();
        }
        if (m_ReadOnly)
        {
            ImGui::SameLine();
            ImGui::TextColored({0.9f, 0.8f, 0.3f, 1.0f}, "(read-only: newer graph version)");
        }
        if (m_Cooking)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("Cooking...");
        }
        if (m_CookError)
            ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Cook error: %s", m_CookError->c_str());
        if (m_Toast && m_ToastRemaining > 0.0f)
            ImGui::TextColored({0.9f, 0.6f, 0.3f, 1.0f}, "Rejected: %s", m_Toast->c_str());

        ImGui::Separator();

        // Layout: a preview + node inspector column on the left, the canvas on the
        // right.
        const f32 sideWidth = 280.0f;
        ImGui::BeginChild("MatSide", ImVec2(sideWidth, 0), ImGuiChildFlags_None);
        {
            const f32 side = PreviewExtent.x;
            if (m_PreviewReady)
                ImGui::Image(m_Preview->GetTextureId(), ImVec2(side, side));
            else
                ImGui::TextUnformatted("Preview loading...");
            ImGui::Separator();
            DrawNodeInspector();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("MatCanvas", ImVec2(0, 0), ImGuiChildFlags_None);
        const bool mutated = DrawCanvas();
        ImGui::EndChild();

        if (mutated)
            MarkDirty();
    }
}
