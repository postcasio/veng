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

    /// @brief Registers the named combo widgets for the project-settings enums.
    ///
    /// CompressionRole and CompressionFormat draw a name-table combo rather than the generic
    /// editable-integer enum widget, mirroring the LightType combo the inspector registers.
    /// The host calls this once at startup so the combos are available to the project-settings
    /// panel regardless of whether a prefab editor is open. Idempotent.
    /// @param editors  The shared editor registry receiving the widget registrations.
    void RegisterCompressionWidgets(Veng::EditorRegistry& editors);

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
        InspectorPanel(Veng::AssetManager& assets, Veng::EditorRegistry& editors,
                       const AssetSourceIndex& sources, PrefabEditContext& ctx);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Inspector"; }
        void OnUI() override;

    private:
        /// @brief Draws the entity-name editor and id readout above the component list.
        /// @param entity The active entity whose name is edited.
        void DrawHeader(Veng::Entity entity);

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

        /// @brief Scratch buffer backing the entity-name input across frames.
        Veng::string m_NameScratch;

        /// @brief The entity m_NameScratch currently mirrors, so a selection change reloads it.
        Veng::Entity m_NameFor = Veng::Entity::Null;

        /// @brief Scratch buffer backing the Add Component search box across frames.
        Veng::string m_AddSearch;
    };
}
