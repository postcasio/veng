#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>
#include <VengEditor/EditorRegistry.h>

#include "material/MaterialPreview.h"

#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class Application;
    class AssetManager;
    class ImGuiLayer;

    namespace Renderer
    {
        class Context;
    }
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Material-instance authoring surface: a parent picker plus a per-field override
    /// toggle over the parent's exposed schema, with a live MaterialPreview.
    ///
    /// Edits a `*.vmatinst.json`. The override surface is exactly the parent's `GetFields()`
    /// exposed subset — toggling a field on adds it to the sparse override set, off reverts it to
    /// the parent default — so the authored surface and the cook-validated surface are the same
    /// set by construction. Any edit recooks the instance off the render thread and refreshes the
    /// preview; Save writes the `*.vmatinst.json`.
    class MaterialInstanceEditorPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the editor for the material instance at @p id / @p sourcePath.
        /// @param app  Application the preview's Offscreen viewport is registered into.
        MaterialInstanceEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                                    const AssetSourceIndex& sources, Veng::Application& app,
                                    Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                                    CookDriver cook);
        ~MaterialInstanceEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

    private:
        /// @brief One authored override slot mirroring a parent exposed field.
        struct OverrideSlot
        {
            /// @brief The parent field name this slot targets.
            Veng::string Name;
            /// @brief True when this field contributes an override (else it inherits the parent default).
            bool Overridden = false;
            /// @brief Whether this is a texture-handle field (else a scalar/vector param).
            bool IsTexture = false;
            /// @brief Component count for a param field (1 = float, 2/3/4 = vecN); 0 for a texture.
            Veng::u32 Components = 0;
            /// @brief Current param value (xyzw used per Components), seeded from the parent default.
            Veng::vec4 Value{0.0f, 0.0f, 0.0f, 1.0f};
            /// @brief Current texture override id for a texture field; null inherits the parent default.
            Veng::AssetId Texture;
        };

        /// @brief Loads the parent material (via the default-instance rule), the schema, and the
        /// authored overrides from the source document.
        /// @return False (logged) on a load failure.
        bool LoadInstance();

        /// @brief Rebuilds the override-slot table from the parent's exposed schema, seeding each
        /// slot's value from the parent default block and the previously authored overrides.
        void BuildSlots();

        /// @brief Reads the `parent` id and `overrides` map from the source document.
        void ReadSource();

        /// @brief Assembles the `*.vmatinst.json` document from the parent id + the toggled overrides.
        [[nodiscard]] Veng::string AssembleDocument() const;

        /// @brief Writes the assembled document to @p target.
        /// @return False (error recorded) on an I/O failure.
        bool WriteDocument(const Veng::path& target);

        /// @brief Marks the instance dirty and arms the debounced recook.
        void MarkDirty();

        /// @brief Writes the temp source and drives the cook, mounting + hot-reloading the result.
        void TriggerCook();

        const AssetSourceIndex& m_Sources;
        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        CookDriver m_Cook;

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        /// @brief Dotfile temp cook source beside the real source; removed on close.
        Veng::path m_TempPath;
        Veng::string m_Title;

        /// @brief The authored parent material id (the override target).
        Veng::AssetId m_ParentId;
        /// @brief The parent material handle, kept resident for its schema + default block.
        Veng::AssetHandle<Veng::MaterialInstance> m_Parent;

        /// @brief Authored overrides parsed from the source, by field name (param bytes / texture id).
        Veng::unordered_map<Veng::string, Veng::vec4> m_AuthoredParams;
        Veng::unordered_map<Veng::string, Veng::AssetId> m_AuthoredTextures;

        /// @brief The per-field override slots driven by the inspector.
        Veng::vector<OverrideSlot> m_Slots;

        Veng::Unique<MaterialPreview> m_Preview;
        bool m_PreviewReady = false;

        Veng::AssetHandle<Veng::MaterialInstance> m_Handle;
        Veng::MountHandle m_Mount;
        bool m_Cooking = false;
        bool m_CookPending = false;
        Veng::f32 m_DebounceRemaining = 0.0f;
        bool m_InstanceDirty = false;
        Veng::optional<Veng::string> m_CookError;
    };
}
