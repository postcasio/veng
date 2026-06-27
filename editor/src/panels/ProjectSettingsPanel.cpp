#include "panels/ProjectSettingsPanel.h"

#include "AssetSourceIndex.h"
#include "EditorIcons.h"
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
#include <sstream>
#include <unordered_map>
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
            (void)DrawFields(&m_Settings, info.Fields, ctx);
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
            if (UI::Button(Icons::Save))
            {
                Save();
            }
            UI::Tooltip("Save project.veng and each configuration's .buildcfg");
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

        const path dir = m_ProjectFile.parent_path();

        // Round-trip the existing project.veng so keys the panel does not own (packs) survive, and
        // each configuration rewrites its *.buildcfg where it already lives (e.g. under configs/).
        nlohmann::json project = nlohmann::json::object();
        {
            std::ifstream in(m_ProjectFile, std::ios::binary);
            if (in)
            {
                std::ostringstream contents;
                contents << in.rdbuf();
                nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                {
                    project = std::move(parsed);
                }
            }
        }

        // Map each existing configuration's name to its referenced relative path, so a save
        // rewrites the *.buildcfg in place rather than beside the project file.
        std::unordered_map<string, string> pathByName;
        if (project.contains("configurations") && project["configurations"].is_array())
        {
            for (const nlohmann::json& entry : project["configurations"])
            {
                if (!entry.is_string())
                {
                    continue;
                }
                const string rel = entry.get<string>();
                std::ifstream cf(dir / rel, std::ios::binary);
                if (!cf)
                {
                    continue;
                }
                std::ostringstream cc;
                cc << cf.rdbuf();
                const nlohmann::json cj = nlohmann::json::parse(cc.str(), nullptr, false);
                if (cj.is_object() && cj.contains("name") && cj["name"].is_string())
                {
                    pathByName[cj["name"].get<string>()] = rel;
                }
            }
        }

        project["activeConfiguration"] = m_Settings.ActiveConfiguration;
        if (m_Settings.StartupLevel.IsValid())
        {
            project["startupLevel"] = m_Settings.StartupLevel.Value;
        }
        else
        {
            project.erase("startupLevel");
        }

        nlohmann::json configurations = nlohmann::json::array();
        for (const BuildConfiguration& config : m_Settings.Configurations)
        {
            // Reuse the existing referenced path; a configuration added in the editor defaults under
            // configs/, matching the example layout.
            const auto it = pathByName.find(config.Name);
            const string rel =
                it != pathByName.end() ? it->second : ("configs/" + config.Name + ".buildcfg");
            configurations.push_back(rel);

            const path outFile = dir / rel;
            std::error_code ec;
            std::filesystem::create_directories(outFile.parent_path(), ec);
            if (!WriteJson(outFile, ConfigToJson(config)))
            {
                m_Error = fmt::format("failed to write {}", outFile.string());
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
