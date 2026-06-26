#include "panels/ProjectSettingsPanel.h"

#include "AssetSourceIndex.h"
#include "FieldWidget.h"
#include "PreviewCapability.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Log.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Reads the format a role resolves to from the fixed RoleToFormat record.
        CompressionFormat RoleFormat(const RoleToFormat& table, CompressionRole role)
        {
            switch (role)
            {
            case CompressionRole::Color:
                return table.Color;
            case CompressionRole::Normal:
                return table.Normal;
            case CompressionRole::Mask:
                return table.Mask;
            case CompressionRole::HDR:
                return table.HDR;
            case CompressionRole::UI:
                return table.UI;
            }
            return table.Color;
        }

        // Serializes one BuildConfiguration to its *.buildcfg JSON shape, the same schema the
        // cooker's ParseBuildConfiguration reads back. Enums are written by name through the
        // shared tables, never by ordinal.
        nlohmann::json ConfigToJson(const BuildConfiguration& config)
        {
            nlohmann::json cfg = nlohmann::json::object();
            cfg["name"] = config.Name;
            cfg["target"] = config.Target;
            cfg["outputSuffix"] = config.OutputSuffix;
            cfg["compressionLevel"] = config.CompressionLevel;

            nlohmann::json formats = nlohmann::json::object();
            for (const CompressionRole role : CompressionRoles)
            {
                formats[std::string(ToString(role))] =
                    std::string(ToString(RoleFormat(config.Formats, role)));
            }
            cfg["formats"] = std::move(formats);
            return cfg;
        }

        bool WriteJson(const path& file, const nlohmann::json& document)
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out << document.dump(4) << '\n';
            return true;
        }
    }

    ProjectSettingsPanel::ProjectSettingsPanel(ProjectSettings& settings, path projectFile,
                                               AssetManager& assets, const EditorRegistry& editors,
                                               const AssetSourceIndex& sources,
                                               const Renderer::Context& context,
                                               PreviewConfigGetter getPreview,
                                               PreviewConfigSetter setPreview)
        : m_Settings(settings), m_ProjectFile(std::move(projectFile)), m_Assets(assets),
          m_Editors(editors), m_Sources(sources), m_Context(context),
          m_GetPreview(std::move(getPreview)), m_SetPreview(std::move(setPreview))
    {
    }

    void ProjectSettingsPanel::OnUI()
    {
        const TypeRegistry& types = m_Assets.GetTypeRegistry();
        const TypeInfo& info = types.Info(types.IdOf<ProjectSettings>());

        // Reflection draws every field; the registered CompressionRole/CompressionFormat combos
        // and the FieldClass::Array add/remove widget make the configuration list editable.
        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        if (auto table = UI::PropertyTable("##projectsettings"))
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Hidden)
                {
                    continue;
                }
                void* fieldPtr = reinterpret_cast<u8*>(&m_Settings) + field.Offset;
                (void)DrawFieldWidget(fieldPtr, field, ctx);
            }
        }

        UI::SeparatorText("Active configuration");

        // A name combo over the configurations is friendlier than typing the active name into
        // the reflected String field; both edit the same ActiveConfiguration string.
        std::vector<string> names;
        names.reserve(m_Settings.Configurations.size());
        for (const BuildConfiguration& config : m_Settings.Configurations)
        {
            names.push_back(config.Name);
        }
        const std::vector<string_view> items(names.begin(), names.end());

        i32 active = -1;
        for (usize i = 0; i < names.size(); ++i)
        {
            if (names[i] == m_Settings.ActiveConfiguration)
            {
                active = static_cast<i32>(i);
                break;
            }
        }
        if (!items.empty())
        {
            i32 selected = active < 0 ? 0 : active;
            if (UI::Combo("##activeconfig", selected, items))
            {
                m_Settings.ActiveConfiguration = names[static_cast<usize>(selected)];
            }
        }
        else
        {
            UI::TextDisabled("No configurations defined");
        }

        DrawPreviewGate();

        UI::Separator();

        const bool canSave = !m_ProjectFile.empty();
        {
            auto disabled = UI::Disabled(!canSave);
            if (UI::Button("Save"))
            {
                Save();
            }
        }
        if (!canSave)
        {
            UI::SameLine();
            UI::TextDisabled("(no project file path)");
        }

        if (m_Error)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Save error: {}", *m_Error));
        }
    }

    void ProjectSettingsPanel::DrawPreviewGate()
    {
        UI::SeparatorText("Live preview");

        // The same capability query both warns about the active configuration and gates the
        // selector — one query, two uses.
        for (const BuildConfiguration& config : m_Settings.Configurations)
        {
            if (config.Name != m_Settings.ActiveConfiguration)
            {
                continue;
            }
            const PreviewCapability cap = IsConfigPreviewable(config, m_Context);
            if (!cap.Previewable)
            {
                UI::TextColored(
                    {0.95f, 0.7f, 0.25f, 1.0f},
                    fmt::format("Active configuration '{}' {}", config.Name, cap.Reason));
            }
            break;
        }

        // The default preview is host-safe (uncompressed) regardless of the selected ship
        // configuration. "Preview as ship config" is the opt-in to that target's real codec
        // artifacts; host-incompatible configurations are greyed out with their reason. Whether
        // any configuration is previewable, the editor stays host-safe and fully editable.
        // Copied, not referenced: a Selectable below mutates the host's opt-in through m_SetPreview
        // mid-loop, which would invalidate a reference into it.
        const optional<string> current = m_GetPreview();
        const string preview = current ? *current : string("Host-safe (uncompressed)");

        usize previewableCount = 0;
        for (const BuildConfiguration& config : m_Settings.Configurations)
        {
            if (IsConfigPreviewable(config, m_Context).Previewable)
            {
                ++previewableCount;
            }
        }

        if (auto combo = UI::ComboBox("Preview as ship config", preview))
        {
            // The host-safe default is always selectable; it is the never-stuck fallback.
            if (UI::Selectable("Host-safe (uncompressed)", !current))
            {
                m_SetPreview(std::nullopt);
            }

            for (const BuildConfiguration& config : m_Settings.Configurations)
            {
                const PreviewCapability cap = IsConfigPreviewable(config, m_Context);
                const bool selected = current && *current == config.Name;

                auto disabled = UI::Disabled(!cap.Previewable);
                const string label =
                    cap.Previewable ? config.Name : fmt::format("{} ({})", config.Name, cap.Reason);
                if (UI::Selectable(label, selected) && cap.Previewable)
                {
                    m_SetPreview(config.Name);
                }
            }
        }

        // The never-stuck fallback: a project whose every configuration targets a codec this GPU
        // cannot sample still previews host-safe, stated plainly so the author is not surprised.
        if (!m_Settings.Configurations.empty() && previewableCount == 0)
        {
            UI::TextDisabled("previewing uncompressed; no build configuration targets this GPU");
        }
    }

    bool ProjectSettingsPanel::Save()
    {
        m_Error.reset();

        // Each configuration writes a sibling <name>.buildcfg; project.veng references them by
        // file name and names the active one. The cooker reads the same *.buildcfg schema.
        const path dir = m_ProjectFile.parent_path();

        nlohmann::json project = nlohmann::json::object();
        project["activeConfiguration"] = m_Settings.ActiveConfiguration;

        nlohmann::json configurations = nlohmann::json::array();
        for (const BuildConfiguration& config : m_Settings.Configurations)
        {
            const string fileName = config.Name + ".buildcfg";
            configurations.push_back(fileName);

            if (!WriteJson(dir / fileName, ConfigToJson(config)))
            {
                m_Error = fmt::format("failed to write {}", (dir / fileName).string());
                Log::Error("Project settings: {}", *m_Error);
                return false;
            }
        }
        project["configurations"] = std::move(configurations);

        if (!WriteJson(m_ProjectFile, project))
        {
            m_Error = fmt::format("failed to write {}", m_ProjectFile.string());
            Log::Error("Project settings: {}", *m_Error);
            return false;
        }

        return true;
    }
}
