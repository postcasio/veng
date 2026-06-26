#pragma once

#include <Veng/Project/ProjectSettings.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class EditorRegistry;

    namespace Renderer
    {
        class Context;
    }
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Reads the ship configuration live preview is opted into, or nullopt for host-safe.
    using PreviewConfigGetter = Veng::function<const Veng::optional<Veng::string>&()>;

    /// @brief Opts live preview into a ship configuration by name, or back to host-safe (nullopt).
    using PreviewConfigSetter = Veng::function<void(Veng::optional<Veng::string>)>;

    /// @brief Host-level panel that lists and edits the project's build configurations.
    ///
    /// Edits the host-owned ProjectSettings — the array of BuildConfigurations and the
    /// ActiveConfiguration selector — through the shared reflection inspector
    /// (DrawFieldWidget / PropertyTable). Reflection draws the rows; the panel supplies the
    /// active-configuration combo over the configurations' names and a Save button that
    /// writes project.veng (and each configuration's *.buildcfg sibling) through the editor's
    /// own nlohmann, using the shared enum⇄name tables. The CompressionRole / CompressionFormat
    /// combos and the configuration-array add/remove widget come from the registered field
    /// widgets and the FieldClass::Array widget the inspector layer provides.
    ///
    /// The panel also drives the host's live-preview gate: an "active configuration not
    /// supported on this GPU" notice and a "preview as ship config" selector that greys out
    /// host-incompatible configurations with a stated reason, over the host's render context
    /// capability queries.
    class ProjectSettingsPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the panel over the host-owned project settings.
        /// @param settings   The host-owned ProjectSettings the panel edits in place.
        /// @param projectFile Absolute path project.veng is saved to (its directory holds the
        ///                    per-configuration *.buildcfg files). Empty disables Save.
        /// @param assets     Asset manager supplying the TypeRegistry the inspector walks.
        /// @param editors    Editor registry supplying the registered enum/array widgets.
        /// @param sources    Manifest source index the inspector's asset pickers read.
        /// @param context    Render context whose enabled features gate preview eligibility.
        /// @param getPreview Reads the host's current preview-config opt-in.
        /// @param setPreview Sets the host's preview-config opt-in (nullopt = host-safe).
        ProjectSettingsPanel(Veng::ProjectSettings& settings, Veng::path projectFile,
                             Veng::AssetManager& assets, const Veng::EditorRegistry& editors,
                             const AssetSourceIndex& sources,
                             const Veng::Renderer::Context& context, PreviewConfigGetter getPreview,
                             PreviewConfigSetter setPreview);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Project Settings"; }
        void OnUI() override;

    private:
        /// @brief Writes project.veng and each configuration's *.buildcfg through nlohmann.
        /// @return False (error recorded in m_Error) on an I/O failure.
        bool Save();

        /// @brief Draws the live-preview gate: the host-capability notice and config selector.
        void DrawPreviewGate();

        Veng::ProjectSettings& m_Settings;
        Veng::path m_ProjectFile;
        Veng::AssetManager& m_Assets;
        const Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;
        const Veng::Renderer::Context& m_Context;
        PreviewConfigGetter m_GetPreview;
        PreviewConfigSetter m_SetPreview;

        /// @brief Last save error, shown inline until the next successful save.
        Veng::optional<Veng::string> m_Error;
    };
}
