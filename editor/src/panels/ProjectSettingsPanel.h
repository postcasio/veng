#pragma once

#include <Veng/Project/ProjectSettings.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;

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
        ProjectSettingsPanel(Veng::ProjectSettings& settings, Veng::path projectFile,
                             Veng::AssetManager& assets, const Veng::EditorRegistry& editors,
                             const AssetSourceIndex& sources);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Project Settings"; }
        void OnUI() override;

    private:
        /// @brief Writes project.veng and each configuration's *.buildcfg through nlohmann.
        /// @return False (error recorded in m_Error) on an I/O failure.
        bool Save();

        Veng::ProjectSettings& m_Settings;
        Veng::path m_ProjectFile;
        Veng::AssetManager& m_Assets;
        const Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;

        /// @brief Last save error, shown inline until the next successful save.
        Veng::optional<Veng::string> m_Error;
    };
}
