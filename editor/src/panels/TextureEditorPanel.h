#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Renderer/Types.h>

#include <VengEditor/CookRequest.h>

#include "AssetEditorPanel.h"
#include "AssetSaveModel.h"

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;

    namespace Renderer
    {
        class Context;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief Off-thread cook callback bound to EditorHost::RequestCook.
    ///
    /// The host shadow-mounts the cooked result and delivers a MountHandle (or
    /// error) back on the main thread.
    using CookDriver = Veng::function<void(const CookRequest&,
                                           Veng::function<void(Veng::Result<Veng::MountHandle>)>)>;

    /// @brief Reads the project's active build configuration (or null for the zero-config state).
    ///
    /// Read each frame so the resolved-format read-out tracks a configuration change made in
    /// the Project Settings panel while this editor is open.
    using ActiveConfigAccessor = Veng::function<const Veng::BuildConfiguration*()>;

    /// @brief Reads the host-clamped configuration the editor's live preview cooks through.
    ///
    /// Host-safe by default and never an unsamplable codec; "preview as ship config" substitutes
    /// a previewable ship configuration. Read each frame so flipping the preview selection
    /// re-cooks the open texture editor, and to label the preview with what it shows.
    using PreviewConfigAccessor = Veng::function<Veng::BuildConfiguration()>;

    /// @brief Docked panel for previewing and editing a .tex.json texture source.
    ///
    /// Shows the decoded texture in a live preview and exposes the sRGB, compression-role, and
    /// sampler settings. It writes nothing until an explicit save: an edit marks the document
    /// dirty, and Save performs the preserve-unknown-keys merge write, then the recook that
    /// refreshes the preview.
    class TextureEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the texture at @p id / @p sourcePath.
        TextureEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                           Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, CookDriver cook,
                           ActiveConfigAccessor activeConfig, PreviewConfigAccessor previewConfig);
        ~TextureEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

        /// @brief Writes the settings back to the .tex.json, then recooks.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true while the settings hold edits not yet written to the source.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Dirty; }

    private:
        /// @brief Editable subset of the .tex.json fields.
        ///
        /// The sampler fields are the engine vocabulary enums; their authoring
        /// spellings come from the shared name tables (Renderer/TypeNames.h) the
        /// importer parses by. The mip filter reuses Filter — its two authored
        /// names and ordinals coincide with MipmapMode's.
        struct Settings
        {
            bool Srgb = true;
            Veng::Renderer::Filter Min = Veng::Renderer::Filter::Linear;
            Veng::Renderer::Filter Mag = Veng::Renderer::Filter::Linear;
            Veng::Renderer::Filter Mipmap = Veng::Renderer::Filter::Linear;
            Veng::Renderer::AddressMode WrapU = Veng::Renderer::AddressMode::Repeat;
            Veng::Renderer::AddressMode WrapV = Veng::Renderer::AddressMode::Repeat;
            /// @brief The authored compression role, or nullopt to fall back to the sRGB guess.
            Veng::optional<Veng::CompressionRole> Role;
        };

        /// @brief Reads the on-disk .tex.json into m_Settings; absent fields keep defaults.
        void LoadSettings();

        /// @brief Patches the settings keys into the existing JSON (preserving unknown keys)
        /// and writes it back with 4-space indent.
        /// @return Empty on success; an I/O error otherwise.
        [[nodiscard]] Veng::VoidResult WriteSettings();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        CookDriver m_Cook;
        ActiveConfigAccessor m_ActiveConfig;
        PreviewConfigAccessor m_PreviewConfig;

        /// @brief Name of the preview configuration the last cook ran through; a change re-cooks.
        Veng::string m_PreviewConfigName;

        Settings m_Settings;

        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::ImGuiTexture> m_Preview;
        Veng::AssetHandle<Veng::Texture> m_Handle;
        Veng::MountHandle m_Mount;

        /// @brief Serialises recooks; a save landing behind one in flight queues rather than drops.
        CookGate m_Gate;

        /// @brief Whether the settings hold edits not yet written to the source.
        bool m_Dirty = false;

        /// @brief Handle is resident but the preview ImGuiTexture has not been (re)created;
        /// creation is deferred to OnUI where the ImGui frame is live.
        bool m_PreviewDirty = false;

        Veng::optional<Veng::string> m_CookError;
    };
}
