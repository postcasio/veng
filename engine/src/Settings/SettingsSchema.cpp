#include <Veng/Settings/SettingsSchema.h>

namespace Veng
{
    Ref<SettingsSchema> SettingsSchema::Create(SettingsSchemaData data)
    {
        return Ref<SettingsSchema>(new SettingsSchema(std::move(data)));
    }

    SettingsSchema::SettingsSchema(SettingsSchemaData data) : m_Data(std::move(data)) {}

    const SettingsCategory* SettingsSchema::FindCategory(const std::string_view id) const
    {
        for (const SettingsCategory& category : m_Data.Categories)
        {
            if (category.Id == id)
            {
                return &category;
            }
        }
        return nullptr;
    }

    const SettingsSetting* SettingsSchema::FindSetting(const std::string_view id) const
    {
        for (const SettingsCategory& category : m_Data.Categories)
        {
            for (const SettingsSetting& setting : category.Settings)
            {
                if (setting.Id == id)
                {
                    return &setting;
                }
            }
        }
        return nullptr;
    }

    const SettingsPreset* SettingsSchema::FindPreset(const std::string_view id) const
    {
        for (const SettingsPreset& preset : m_Data.Presets)
        {
            if (preset.Id == id)
            {
                return &preset;
            }
        }
        return nullptr;
    }
}
