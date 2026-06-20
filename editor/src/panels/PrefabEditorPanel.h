#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>

#include "AssetEditorPanel.h"
#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    class ImGuiLayer;
    class Prefab;
    class Scene;
    class TypeRegistry;

    namespace Renderer
    {
        class Context;
    }
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Asset editor for a prefab: a private dockspace hosting a scene viewport,
    /// an entity-hierarchy explorer, and a reflection inspector over one spawned Scene.
    ///
    /// On open the prefab is loaded and spawned into a fresh Scene the document owns; a
    /// default directional light is added when the prefab carries none so the content is
    /// lit. The explorer drives selection, the inspector edits the selected entity's
    /// components, and the viewport renders the live scene — all sharing one
    /// PrefabEditContext.
    class PrefabEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the prefab at @p id.
        /// @param id        The prefab asset to edit.
        /// @param context   Render context for the viewport's SceneRenderer.
        /// @param assets    Asset manager the prefab and its dependencies load through.
        /// @param imgui     ImGui layer the viewport registers its render target with.
        /// @param types     Type registry the spawned Scene pools components against.
        /// @param editors   Editor registry for inspector field-widget overrides.
        /// @param sources   Manifest source index for the inspector's asset pickers.
        PrefabEditorPanel(Veng::AssetId id, Veng::Renderer::Context& context,
                          Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                          Veng::TypeRegistry& types, Veng::EditorRegistry& editors,
                          const AssetSourceIndex& sources);
        ~PrefabEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }

    protected:
        /// @brief Splits the dockspace into explorer (left), viewport (center), inspector (right).
        void BuildDefaultLayout(Veng::u32 dockspaceId) override;

    private:
        /// @brief Loads and spawns the prefab, adding a default light when none is present.
        void BuildScene(Veng::Renderer::Context& context, Veng::AssetManager& assets);

        Veng::AssetId m_Id;
        Veng::string m_Title;

        Veng::AssetHandle<Veng::Prefab> m_Prefab;
        Veng::Unique<Veng::Scene> m_Scene;
        PrefabEditContext m_Context;

        Veng::usize m_ExplorerChild = 0;
        Veng::usize m_ViewportChild = 0;
        Veng::usize m_InspectorChild = 0;
    };
}
