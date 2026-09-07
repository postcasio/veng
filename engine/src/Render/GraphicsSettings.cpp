#include <Veng/Render/GraphicsSettings.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Log.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        // A scalar choice (render scale, a slider) survives a JSON text round-trip with tiny
        // rounding, so a preset match compares floats within a small tolerance rather than bit-exact.
        constexpr f32 ScalarMatchEpsilon = 1e-4f;

        bool ScalarsMatch(const f32 a, const f32 b)
        {
            return std::fabs(a - b) <= ScalarMatchEpsilon;
        }

        Result<string> ReadFileText(const path& filePath)
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file)
            {
                return std::unexpected(fmt::format("cannot open '{}'", filePath.string()));
            }
            string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
            if (file.bad())
            {
                return std::unexpected(fmt::format("cannot read '{}'", filePath.string()));
            }
            return content;
        }
    }

    GraphicsSettings::GraphicsSettings(GraphicsSettingsInfo info)
        : m_Schema(info.Schema), m_Types(info.Types), m_ConfigPath(std::move(info.ConfigPath))
    {
        ResetToDefaults();
    }

    const GraphicsChoice* GraphicsSettings::FindChoice(const std::string_view settingId) const
    {
        for (const GraphicsChoice& choice : m_Choices.Choices)
        {
            if (choice.SettingId == settingId)
            {
                return &choice;
            }
        }
        return nullptr;
    }

    void GraphicsSettings::SetChoice(const std::string_view settingId,
                                     const std::string_view optionId, const f32 scalarValue)
    {
        for (GraphicsChoice& choice : m_Choices.Choices)
        {
            if (choice.SettingId == settingId)
            {
                choice.OptionId = string(optionId);
                choice.ScalarValue = scalarValue;
                return;
            }
        }
        m_Choices.Choices.push_back(GraphicsChoice{.SettingId = string(settingId),
                                                   .OptionId = string(optionId),
                                                   .ScalarValue = scalarValue});
    }

    void GraphicsSettings::DropStaleChoices()
    {
        if (m_Schema == nullptr)
        {
            return;
        }
        std::erase_if(m_Choices.Choices, [this](const GraphicsChoice& choice)
                      { return m_Schema->FindSetting(choice.SettingId) == nullptr; });
    }

    string GraphicsSettings::GetChosenOption(const std::string_view settingId) const
    {
        if (const GraphicsChoice* choice = FindChoice(settingId))
        {
            return choice->OptionId;
        }
        if (m_Schema != nullptr)
        {
            if (const GraphicsSetting* setting = m_Schema->FindSetting(settingId);
                setting != nullptr && setting->Kind == GraphicsSettingKind::Discrete &&
                !setting->Options.empty())
            {
                const u32 index =
                    setting->DefaultOption < setting->Options.size() ? setting->DefaultOption : 0;
                return setting->Options[index].Id;
            }
        }
        return {};
    }

    f32 GraphicsSettings::GetChosenScalar(const std::string_view settingId) const
    {
        if (const GraphicsChoice* choice = FindChoice(settingId))
        {
            return choice->ScalarValue;
        }
        if (m_Schema != nullptr)
        {
            if (const GraphicsSetting* setting = m_Schema->FindSetting(settingId))
            {
                return setting->DefaultValue;
            }
        }
        return 0.0f;
    }

    void GraphicsSettings::SetChosenOption(const std::string_view settingId,
                                           const std::string_view optionId)
    {
        SetChoice(settingId, optionId, 0.0f);
    }

    void GraphicsSettings::SetChosenScalar(const std::string_view settingId, const f32 value)
    {
        SetChoice(settingId, {}, value);
    }

    VoidResult GraphicsSettings::ApplyPreset(const std::string_view presetId)
    {
        if (m_Schema == nullptr)
        {
            return std::unexpected(string("cannot apply a preset without a schema"));
        }
        const GraphicsPreset* preset = m_Schema->FindPreset(presetId);
        if (preset == nullptr)
        {
            return std::unexpected(fmt::format("no such preset '{}'", presetId));
        }

        for (const GraphicsPresetEntry& entry : preset->Entries)
        {
            // The one preset-eligible built-in a preset may carry; the display-identity built-ins
            // are never touched here.
            if (entry.SettingId == GraphicsRenderScaleBuiltinId)
            {
                m_Choices.Display.RenderScale = entry.ScalarValue;
                continue;
            }
            SetChoice(entry.SettingId, entry.OptionId, entry.ScalarValue);
        }
        m_Choices.ActivePreset = string(presetId);
        return {};
    }

    void GraphicsSettings::ResetToDefaults()
    {
        m_Choices.Choices.clear();
        m_Choices.ActivePreset.clear();
        m_Choices.Display = BuiltinDisplayChoices{};
        m_Choices.Version = GraphicsChoicesVersion;

        if (m_Schema != nullptr && !m_Schema->GetDefaultPreset().empty())
        {
            // A default preset that names no known preset leaves the settings at their schema
            // defaults (each absent choice reads its default) and ActivePreset empty.
            const VoidResult applied = ApplyPreset(m_Schema->GetDefaultPreset());
            if (!applied)
            {
                Log::Warn("graphics settings: default preset '{}' is not declared; leaving "
                          "settings at their schema defaults",
                          m_Schema->GetDefaultPreset());
            }
        }
    }

    bool GraphicsSettings::PresetMatches(const GraphicsPreset& preset) const
    {
        for (const GraphicsPresetEntry& entry : preset.Entries)
        {
            if (entry.SettingId == GraphicsRenderScaleBuiltinId)
            {
                if (!ScalarsMatch(m_Choices.Display.RenderScale, entry.ScalarValue))
                {
                    return false;
                }
                continue;
            }

            const GraphicsSetting* setting =
                m_Schema != nullptr ? m_Schema->FindSetting(entry.SettingId) : nullptr;
            if (setting == nullptr)
            {
                // An entry the schema no longer declares cannot disqualify a match.
                continue;
            }
            if (setting->Kind == GraphicsSettingKind::Discrete)
            {
                if (GetChosenOption(entry.SettingId) != entry.OptionId)
                {
                    return false;
                }
            }
            else if (!ScalarsMatch(GetChosenScalar(entry.SettingId), entry.ScalarValue))
            {
                return false;
            }
        }
        return true;
    }

    optional<string> GraphicsSettings::MatchingPreset() const
    {
        if (m_Schema == nullptr)
        {
            return std::nullopt;
        }
        for (const GraphicsPreset& preset : m_Schema->GetPresets())
        {
            if (PresetMatches(preset))
            {
                return preset.Id;
            }
        }
        return std::nullopt;
    }

    VoidResult GraphicsSettings::Load()
    {
        // Start from the schema/engine defaults, then overlay whatever the file supplies. A field
        // the file omits keeps its default, which is what makes a newly-added setting gain its
        // default on an older file.
        ResetToDefaults();

        if (m_ConfigPath.empty())
        {
            return {};
        }

        std::error_code ec;
        if (!std::filesystem::exists(m_ConfigPath, ec) || ec)
        {
            // A missing file is the ordinary first-run path, not an error.
            return {};
        }

        const Result<string> text = ReadFileText(m_ConfigPath);
        if (!text)
        {
            Log::Warn("graphics settings: {}; using defaults", text.error());
            return std::unexpected(text.error());
        }

        const nlohmann::json doc =
            nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object())
        {
            Log::Warn("graphics settings: '{}' is not valid JSON; using defaults",
                      m_ConfigPath.string());
            return std::unexpected(fmt::format("'{}' is not valid JSON", m_ConfigPath.string()));
        }

        VE_ASSERT(m_Types != nullptr, "GraphicsSettings::Load requires a type registry");
        GraphicsChoices loaded;
        const VoidResult read =
            JsonReadFields(&loaded, m_Types->Info(TypeIdOf<GraphicsChoices>()), doc, *m_Types, {},
                           /*allowUnknownFields=*/true);
        if (!read)
        {
            Log::Warn("graphics settings: '{}' is malformed ({}); using defaults",
                      m_ConfigPath.string(), read.error());
            ResetToDefaults();
            return std::unexpected(read.error());
        }

        m_Choices = std::move(loaded);
        DropStaleChoices();
        // A migrated document is re-stamped so a later save carries the current version.
        m_Choices.Version = GraphicsChoicesVersion;
        return {};
    }

    VoidResult GraphicsSettings::Save() const
    {
        if (m_ConfigPath.empty())
        {
            Log::Warn("graphics settings: no config path; not saving");
            return std::unexpected(string("no config path set"));
        }
        VE_ASSERT(m_Types != nullptr, "GraphicsSettings::Save requires a type registry");

        GraphicsChoices out = m_Choices;
        out.Version = GraphicsChoicesVersion;

        const nlohmann::json doc =
            JsonWriteFields(&out, m_Types->Info(TypeIdOf<GraphicsChoices>()), *m_Types);
        const string text = doc.dump(2);
        const auto* bytes = reinterpret_cast<const u8*>(text.data());
        return WriteFileAtomic(m_ConfigPath, std::span<const u8>(bytes, text.size()));
    }
}
