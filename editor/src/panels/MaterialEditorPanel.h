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

struct ImNodesEditorContext;

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

    /// @brief Node-based material editor: imnodes canvas, node-property inspector,
    /// and a live MaterialPreview sphere.
    ///
    /// On open, parses the source's "_editor" graph block or synthesizes a default
    /// graph from the flat field table. Any edit recooks the material off the render
    /// thread and refreshes the preview. The graph is the source of truth, compiled
    /// into a .vmat field list on each cook.
    class MaterialEditorPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the editor for the material at @p id / @p sourcePath.
        MaterialEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                            const AssetSourceIndex& sources, Veng::Renderer::Context& context,
                            Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                            Veng::EditorRegistry& editors, CookDriver cook);
        ~MaterialEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;
        void OnRender(Veng::Renderer::CommandBuffer& cmd) override;

    private:
        /// @brief Loads the material synchronously, populating m_Fields and shader ids.
        /// @return False (logged) if the material fails to load.
        bool LoadInterface();

        /// @brief Builds the catalog + node types, then reads the "_editor" block or
        /// synthesizes a default graph. A newer graph version opens read-only.
        void BuildGraph();

        /// @brief Returns the shader interface view over the panel's owned field table.
        [[nodiscard]] MaterialShaderInterface Interface() const;

        /// @brief Projects the graph through imnodes for one frame, translating gestures
        /// into graph mutations.
        /// @return True if a mutation occurred.
        bool DrawCanvas();

        /// @brief Draws the selected node's properties through the shared field widgets.
        /// @return True if a property edit occurred.
        bool DrawNodeInspector();

        /// @brief Marks the graph dirty and arms the debounced recook.
        void MarkDirty();

        /// @brief Writes the temp .vmat and drives the cook driver.
        ///
        /// A compile or I/O error is recorded in m_CookError and logged.
        void TriggerCook();

        /// @brief Assembles the patched .vmat JSON from the current graph.
        ///
        /// Regenerates "fields" and "_editor", preserves "shaders" and unknown keys.
        /// @return The document string, or nullopt on compile failure (error recorded).
        [[nodiscard]] Veng::optional<Veng::string> AssembleVmat() const;

        /// @brief Writes the assembled document to @p target.
        /// @return False (error recorded) on compile or I/O failure.
        bool WriteVmat(const Veng::path& target);

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        /// @brief Dotfile temp cook source beside the real source; removed on close.
        Veng::path m_TempPath;
        Veng::string m_Title;

        const AssetSourceIndex& m_Sources;
        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        Veng::EditorRegistry& m_Editors;
        CookDriver m_Cook;

        /// @brief Reflected field table owned by the panel so the shader interface span stays valid.
        Veng::vector<Veng::MaterialField> m_Fields;
        Veng::AssetId m_VertexShader;
        Veng::AssetId m_FragmentShader;

        /// @brief Domain selects the MaterialOutput pin contract and is re-emitted into the .vmat.
        Veng::MaterialDomain m_Domain = Veng::MaterialDomain::Surface;

        /// @brief Node type catalog; must outlive the graph.
        NodeCatalog m_Catalog;
        MaterialNodeTypes m_Types;
        Veng::Unique<NodeGraph> m_Graph;

        /// @brief Per-panel imnodes canvas state (panning, node positions).
        ///
        /// The library-singleton imnodes context is owned by libveng's ImGuiLayer;
        /// each panel owns only its canvas slice so multiple editors stay isolated.
        ImNodesEditorContext* m_NodeEditorContext = nullptr;

        Veng::Unique<MaterialPreview> m_Preview;
        bool m_PreviewReady = false;

        /// @brief True when the loaded graph version is newer than the editor supports;
        /// Save is disabled and the graph is never regenerated.
        bool m_ReadOnly = false;

        Veng::AssetHandle<Veng::Material> m_Handle;
        Veng::MountHandle m_Mount;
        bool m_Cooking = false;
        bool m_CookPending = false;
        Veng::f32 m_DebounceRemaining = 0.0f;
        /// @brief True when a fresh handle is resident and ready to swap into the preview.
        bool m_MaterialDirty = false;
        Veng::optional<Veng::string> m_CookError;

        /// @brief Transient rejected-connection notice; displayed while m_ToastRemaining > 0.
        Veng::optional<Veng::string> m_Toast;
        Veng::f32 m_ToastRemaining = 0.0f;
    };
}
