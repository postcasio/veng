#pragma once

#include <Veng/Settings/SettingsSchema.h>

namespace Veng
{
    /// @brief The graphics-domain spellings of the shared settings-schema types.
    ///
    /// Graphics is the first domain over the domain-neutral settings core (Veng/Settings/). These
    /// aliases keep the historical `Graphics*` names — the ones a graphics resolver, settings
    /// controller, and cooked `.gfxschema.json` asset are written against — a stable compatibility
    /// surface over the generalized `Settings*` types, which are the same types under new names.
    using GraphicsSchema = SettingsSchema;
    /// @brief Alias of SettingsSchemaData.
    using GraphicsSchemaData = SettingsSchemaData;
    /// @brief Alias of SettingsCategory.
    using GraphicsCategory = SettingsCategory;
    /// @brief Alias of SettingsSetting.
    using GraphicsSetting = SettingsSetting;
    /// @brief Alias of SettingsOption.
    using GraphicsOption = SettingsOption;
    /// @brief Alias of SettingsPreset.
    using GraphicsPreset = SettingsPreset;
    /// @brief Alias of SettingsPresetEntry.
    using GraphicsPresetEntry = SettingsPresetEntry;
    /// @brief Alias of SettingsSettingKind (its Discrete/Scalar enumerators reached through it).
    using GraphicsSettingKind = SettingsSettingKind;
}
