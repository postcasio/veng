#include "panels/ProjectSettingsPanel.h"

#include "AssetSourceIndex.h"
#include "FieldWidget.h"

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
                                               const AssetSourceIndex& sources)
        : m_Settings(settings), m_ProjectFile(std::move(projectFile)), m_Assets(assets),
          m_Editors(editors), m_Sources(sources)
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
