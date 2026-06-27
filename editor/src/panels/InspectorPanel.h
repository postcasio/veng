#pragma once

#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    struct TypeInfo;
}

namespace VengEditor
{
    class AssetSourceIndex;
    class CommandStack;

    /// @brief Reflection-driven property inspector for the prefab editor's active entity.
    ///
    /// Reads the active entity from the shared PrefabEditContext and walks its components
    /// through the TypeRegistry, drawing a property-table widget per FieldClass for every
    /// reflected field. The header offers an editable entity name; each component header
    /// offers remove / reset-to-default; an Add Component popup lists the registered
    /// component types not already present. A custom widget registered in the EditorRegistry
    /// overrides the built-in for a given field TypeId.
    class InspectorPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the inspector over the document's edit context.
        /// @param assets   Asset manager for resolving handle widgets.
        /// @param editors  Editor registry for field-widget overrides.
        /// @param sources  Manifest source index for the asset pickers.
        /// @param ctx      Shared document context supplying the Scene and selection.
        /// @param commands The document's undo/redo stack every edit is pushed through.
        InspectorPanel(Veng::AssetManager& assets, Veng::EditorRegistry& editors,
                       const AssetSourceIndex& sources, PrefabEditContext& ctx,
                       CommandStack& commands);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Inspector"; }
        void OnUI() override;

    private:
        /// @brief Draws one component's header, context menu, and field rows.
        ///
        /// Queues a remove request rather than removing inline, so a structural change
        /// never happens mid-ForEachComponent.
        /// @param entity        The owning entity.
        /// @param id            The component's TypeId.
        /// @param component     Pointer to the component bytes.
        /// @param outRemoveId   Set to @p id when the user requests removal.
        /// @param outRemove     Set true when the user requests removal.
        void DrawComponent(Veng::Entity entity, Veng::TypeId id, void* component,
                           Veng::TypeId& outRemoveId, bool& outRemove);

        /// @brief Draws the Add Component button, popup, search box, and candidate list.
        ///
        /// Adding a component runs outside ForEachComponent, so it is applied immediately.
        /// @param entity The entity to add the chosen component to.
        void DrawAddComponent(Veng::Entity entity);

        Veng::AssetManager& m_Assets;
        Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;
        PrefabEditContext& m_Ctx;
        CommandStack& m_Commands;

        /// @brief An in-progress field edit coalesced into one command across a continuous drag.
        ///
        /// A Drag/Slider widget reports "changed" every frame it is held, so a naive push would
        /// record one command per frame. Instead the first changed frame captures the component's
        /// pre-edit bytes here; while the edit continues the snapshot is held; when no widget is
        /// active any longer the accumulated edit (pre-edit bytes → current bytes) is pushed as one
        /// EditField. So a whole drag is one undo step.
        struct PendingEdit
        {
            /// @brief The entity whose component is being edited.
            Veng::Entity Entity = Veng::Entity::Null;
            /// @brief The component's TypeId.
            Veng::TypeId Type = Veng::InvalidTypeId;
            /// @brief WriteFields bytes of the component captured the frame the edit began.
            Veng::vector<Veng::u8> Before;
        };
        /// @brief The coalescing field edit in flight, or nullopt when none is being made.
        Veng::optional<PendingEdit> m_PendingEdit;

        /// @brief Scratch buffer backing the Add Component search box across frames.
        Veng::string m_AddSearch;
    };
}
