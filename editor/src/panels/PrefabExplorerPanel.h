#pragma once

#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace VengEditor
{
    /// @brief Entity-hierarchy explorer for the prefab editor.
    ///
    /// Walks the document's Scene as a parent/child tree and drives the shared
    /// selection: clicking an entity selects it for the inspector. The tree is the
    /// editor's surface for the scene's structure.
    class PrefabExplorerPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the explorer over the document's edit context.
        /// @param ctx  Shared document context supplying the Scene and selection.
        explicit PrefabExplorerPanel(PrefabEditContext& ctx);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Hierarchy"; }
        void OnImGui() override;

    private:
        /// @brief Draws @p entity and its child subtree as selectable tree nodes.
        void DrawEntity(Veng::Entity entity);

        PrefabEditContext& m_Ctx;

        /// @brief Per-frame parent → children adjacency, rebuilt each OnImGui from the scene.
        Veng::vector<Veng::Entity> m_Roots;
        Veng::unordered_map<Veng::Entity, Veng::vector<Veng::Entity>> m_Children;
    };
}
