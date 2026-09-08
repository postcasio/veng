#include <Veng/Settings/SettingsStore.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Log.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        // A scalar choice (render scale, a slider) survives a JSON text round-trip with tiny
        // rounding, so a preset match compares floats within a small tolerance rather than bit-exact.
        constexpr f32 ScalarMatchEpsilon = 1e-4f;

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

    bool SettingsStoreBase::ScalarsMatch(const f32 a, const f32 b)
    {
        return std::fabs(a - b) <= ScalarMatchEpsilon;
    }

    const SettingsChoice* SettingsStoreBase::FindChoice(const std::string_view settingId) const
    {
        for (const SettingsChoice& choice : AccessChoices())
        {
            if (choice.SettingId == settingId)
            {
                return &choice;
            }
        }
        return nullptr;
    }

    void SettingsStoreBase::SetChoice(const std::string_view settingId,
                                      const std::string_view optionId, const f32 scalarValue)
    {
        for (SettingsChoice& choice : AccessChoices())
        {
            if (choice.SettingId == settingId)
            {
                choice.OptionId = string(optionId);
                choice.ScalarValue = scalarValue;
                return;
            }
        }
        AccessChoices().push_back(SettingsChoice{.SettingId = string(settingId),
                                                 .OptionId = string(optionId),
                                                 .ScalarValue = scalarValue});
    }

    void SettingsStoreBase::DropStaleChoices()
    {
        if (m_Schema == nullptr)
        {
            return;
        }
        std::erase_if(AccessChoices(), [this](const SettingsChoice& choice)
                      { return m_Schema->FindSetting(choice.SettingId) == nullptr; });
    }

    string SettingsStoreBase::GetChosenOption(const std::string_view settingId) const
    {
        if (const SettingsChoice* choice = FindChoice(settingId))
        {
            return choice->OptionId;
        }
        if (m_Schema != nullptr)
        {
            if (const SettingsSetting* setting = m_Schema->FindSetting(settingId);
                setting != nullptr && setting->Kind == SettingsSettingKind::Discrete &&
                !setting->Options.empty())
            {
                const u32 index =
                    setting->DefaultOption < setting->Options.size() ? setting->DefaultOption : 0;
                return setting->Options[index].Id;
            }
        }
        return {};
    }

    f32 SettingsStoreBase::GetChosenScalar(const std::string_view settingId) const
    {
        if (const SettingsChoice* choice = FindChoice(settingId))
        {
            return choice->ScalarValue;
        }
        if (m_Schema != nullptr)
        {
            if (const SettingsSetting* setting = m_Schema->FindSetting(settingId))
            {
                return setting->DefaultValue;
            }
        }
        return 0.0f;
    }

    void SettingsStoreBase::SetChosenOption(const std::string_view settingId,
                                            const std::string_view optionId)
    {
        SetChoice(settingId, optionId, 0.0f);
    }

    void SettingsStoreBase::SetChosenScalar(const std::string_view settingId, const f32 value)
    {
        SetChoice(settingId, {}, value);
    }

    VoidResult SettingsStoreBase::ApplyPreset(const std::string_view presetId)
    {
        if (m_Schema == nullptr)
        {
            return std::unexpected(string("cannot apply a preset without a schema"));
        }
        const SettingsPreset* preset = m_Schema->FindPreset(presetId);
        if (preset == nullptr)
        {
            return std::unexpected(fmt::format("no such preset '{}'", presetId));
        }

        for (const SettingsPresetEntry& entry : preset->Entries)
        {
            // A domain built-in (the graphics render scale) is routed by the domain hook; every
            // other entry is a schema setting stored as an ordinary keyed choice.
            if (ApplyBuiltinPresetEntry(entry))
            {
                continue;
            }
            SetChoice(entry.SettingId, entry.OptionId, entry.ScalarValue);
        }
        AccessActivePreset() = string(presetId);
        return {};
    }

    void SettingsStoreBase::ResetToDefaults()
    {
        AccessChoices().clear();
        AccessActivePreset().clear();
        ResetBuiltinsToDefaults();
        StampDocVersion();

        if (m_Schema != nullptr && !m_Schema->GetDefaultPreset().empty())
        {
            // A default preset that names no known preset leaves the settings at their schema
            // defaults (each absent choice reads its default) and ActivePreset empty.
            const VoidResult applied = ApplyPreset(m_Schema->GetDefaultPreset());
            if (!applied)
            {
                Log::Warn(
                    "settings store: default preset '{}' is not declared; leaving settings at "
                    "their schema defaults",
                    m_Schema->GetDefaultPreset());
            }
        }
    }

    bool SettingsStoreBase::PresetMatches(const SettingsPreset& preset) const
    {
        for (const SettingsPresetEntry& entry : preset.Entries)
        {
            if (const optional<bool> builtin = MatchBuiltinPresetEntry(entry))
            {
                if (!*builtin)
                {
                    return false;
                }
                continue;
            }

            const SettingsSetting* setting =
                m_Schema != nullptr ? m_Schema->FindSetting(entry.SettingId) : nullptr;
            if (setting == nullptr)
            {
                // An entry the schema no longer declares cannot disqualify a match.
                continue;
            }
            if (setting->Kind == SettingsSettingKind::Discrete)
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

    optional<string> SettingsStoreBase::MatchingPreset() const
    {
        if (m_Schema == nullptr)
        {
            return std::nullopt;
        }
        for (const SettingsPreset& preset : m_Schema->GetPresets())
        {
            if (PresetMatches(preset))
            {
                return preset.Id;
            }
        }
        return std::nullopt;
    }

    VoidResult SettingsStoreBase::Load()
    {
        // Start from the schema defaults, then overlay whatever the file supplies. A field the file
        // omits keeps its default, which is what makes a newly-added setting gain its default on an
        // older file.
        ResetToDefaults();
        m_LoadedFromFile = false;

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
        // The file is present: a returning install, whatever its contents parse to below.
        m_LoadedFromFile = true;

        const Result<string> text = ReadFileText(m_ConfigPath);
        if (!text)
        {
            Log::Warn("settings store: {}; using defaults", text.error());
            return std::unexpected(text.error());
        }

        const nlohmann::json doc =
            nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object())
        {
            Log::Warn("settings store: '{}' is not valid JSON; using defaults",
                      m_ConfigPath.string());
            return std::unexpected(fmt::format("'{}' is not valid JSON", m_ConfigPath.string()));
        }

        VE_ASSERT(m_Types != nullptr, "SettingsStore::Load requires a type registry");
        // Overlay onto a fresh struct-default document, so an omitted key keeps its default.
        SetDocToStructDefaults();
        const VoidResult read = JsonReadFields(DocData(), m_Types->Info(DocTypeId()), doc, *m_Types,
                                               {}, /*allowUnknownFields=*/true);
        if (!read)
        {
            Log::Warn("settings store: '{}' is malformed ({}); using defaults",
                      m_ConfigPath.string(), read.error());
            ResetToDefaults();
            return std::unexpected(read.error());
        }

        DropStaleChoices();
        // A migrated document is re-stamped so a later save carries the current version.
        StampDocVersion();
        return {};
    }

    VoidResult SettingsStoreBase::Save() const
    {
        if (m_ConfigPath.empty())
        {
            Log::Warn("settings store: no config path; not saving");
            return std::unexpected(string("no config path set"));
        }
        VE_ASSERT(m_Types != nullptr, "SettingsStore::Save requires a type registry");

        const nlohmann::json doc = JsonWriteFields(DocData(), m_Types->Info(DocTypeId()), *m_Types);
        const string text = doc.dump(2);
        const auto* bytes = reinterpret_cast<const u8*>(text.data());
        return WriteFileAtomic(m_ConfigPath, std::span<const u8>(bytes, text.size()));
    }
}
