#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>
#include <VengEditor/EditorRegistry.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include "material/MaterialCatalog.h"
#include "material/MaterialPreview.h"
#include "material/MaterialShaderInterface.h"

#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
    }
}

namespace VengEditor
{
    class AssetSourceIndex;

    // The node-based material editor: an imnodes canvas over the generic
    // NodeGraph, a node-property inspector reusing the shared field widgets, and a
    // live MaterialPreview sphere. Opened by double-clicking a material; on open
    // it parses the source's "_editor" graph block (or synthesizes a default graph
    // from the flat field table), and any edit recooks the material off the render
    // thread and refreshes the preview. The panel owns no model truth — the graph
    // is the source of truth, compiled into a .vmat field list on each cook.
    class MaterialEditorPanel final : public EditorPanel
    {
    public:
        MaterialEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                            const AssetSourceIndex& sources, Veng::Renderer::Context& context,
                            Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                            Veng::EditorRegistry& editors, CookDriver cook);
        ~MaterialEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnImGui() override;
        void OnRender(Veng::Renderer::CommandBuffer& cmd) override;

    private:
        // Load the material synchronously to read its reflected field table + shader
        // ids into m_Fields (kept resident so the shader interface span is stable).
        // Returns false (logged) when the material fails to load.
        bool LoadInterface();

        // Build the catalog + node types from the current shader interface, then
        // either read the source's "_editor" graph block or synthesize a default
        // graph from the flat field table. A "_editor" version newer than the
        // editor's opens read-only (Save disabled), never regenerating from a
        // degraded parse.
        void BuildGraph();

        // The shader interface view over the panel's owned field-table copy.
        [[nodiscard]] MaterialShaderInterface Interface() const;

        // Project the graph through imnodes for one frame and translate the user's
        // gestures (link create/destroy, node drag, delete, add-node menu) into the
        // Layer-1 mutation vocabulary. Returns true when a mutation occurred.
        bool DrawCanvas();

        // Draw the selected node's properties through the shared field widgets.
        // Returns true when a property edit occurred.
        bool DrawNodeInspector();

        // Mark the graph dirty and arm the debounced recook.
        void MarkDirty();

        // Compile the graph, write the temp .vmat next to the real source, and drive
        // the cook driver. A compile/IO error is recorded and logged.
        void TriggerCook();

        // Assemble the patched .vmat JSON (regenerated "fields", a refreshed
        // "_editor" graph block, preserved "shaders" + unknown keys) from the
        // current graph, reading sourcePath as the base document. Returns nullopt
        // (error recorded) on a compile failure.
        [[nodiscard]] Veng::optional<Veng::string> AssembleVmat() const;

        // Write the patched document to the given path. Returns false (error
        // recorded) on a compile/IO failure.
        bool WriteVmat(const Veng::path& target);

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::path m_TempPath; // the dotfile temp cook source, removed on close
        Veng::string m_Title;

        const AssetSourceIndex& m_Sources;
        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        Veng::EditorRegistry& m_Editors;
        CookDriver m_Cook;

        // The loaded material's reflected field table + shader ids, owned so the
        // shader interface span stays valid for the catalog and compiler.
        Veng::vector<Veng::MaterialField> m_Fields;
        Veng::AssetId m_VertexShader;
        Veng::AssetId m_FragmentShader;

        // Built per loaded material (MaterialOutput pins derive from the field
        // table). The catalog must outlive the graph.
        NodeCatalog m_Catalog;
        MaterialNodeTypes m_Types;
        Veng::Unique<NodeGraph> m_Graph;

        Veng::Unique<MaterialPreview> m_Preview;
        bool m_PreviewReady = false;

        // A "_editor" version newer than the editor opened this read-only: Save is
        // disabled and the graph is never regenerated.
        bool m_ReadOnly = false;

        // The cook/hot-reload state, mirroring the texture editor.
        Veng::AssetHandle<Veng::Material> m_Handle;
        Veng::MountHandle m_Mount;
        bool m_Cooking = false;
        bool m_CookPending = false;
        Veng::f32 m_DebounceRemaining = 0.0f;
        bool m_MaterialDirty = false; // a fresh handle is resident; swap into the preview
        Veng::optional<Veng::string> m_CookError;

        // A transient rejected-connection notice shown for a short while.
        Veng::optional<Veng::string> m_Toast;
        Veng::f32 m_ToastRemaining = 0.0f;
    };
}
