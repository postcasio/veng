#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief The reserved built-in setting id a preset may name outside a domain's schema.
    ///
    /// A preset entry's SettingId is either a schema setting id or this one reserved id — the
    /// render-scale display built-in (defined by the engine's display group, not the schema), which
    /// is the one preset-eligible built-in a preset may carry. Any other built-in id (a
    /// display-identity id, brightness/gamma) or an unknown id is rejected at cook. It lives beside
    /// the schema types because the schema validation enforces the reservation; only the graphics
    /// display group gives it effect.
    inline constexpr std::string_view GraphicsRenderScaleBuiltinId = "render_scale";

    /// @brief Which of a setting's two shapes a SettingsSetting is.
    ///
    /// A setting is one of exactly two kinds; the kind selects which of the struct's fields are
    /// meaningful. A domain's own built-ins (a display group's resolution/monitor/refresh, say) are
    /// not schema entries and have no kind here.
    enum class SettingsSettingKind : u32
    {
        /// @brief A closed option list the author fully declares; the user picks only from it.
        ///
        /// The Options list and DefaultOption index are meaningful; the scalar fields are not.
        Discrete = 0,
        /// @brief A continuous quantity with a min/max/step and a default value.
        ///
        /// The Min/Max/Step/DefaultValue fields are meaningful; Options and DefaultOption are not.
        Scalar = 1,
    };

    /// @brief One selectable option of a discrete setting.
    ///
    /// An option is an *identity*, not a value: its concrete meaning is the consuming game's,
    /// resolved in code. Id is what a resolver switches on and what a store persists, so it is
    /// stable; Label is display-only.
    struct SettingsOption
    {
        /// @brief Stable option identifier the resolver switches on and the store persists.
        string Id;
        /// @brief Display-only label shown in the settings UI.
        string Label;
    };

    /// @brief One adjustable setting: a discrete option list or a continuous scalar.
    ///
    /// A tagged union expressed as a reflected struct — Kind selects which fields are meaningful.
    /// A Discrete setting reads Options and DefaultOption; a Scalar setting reads
    /// Min/Max/Step/DefaultValue. The record carries every field regardless of kind, so the schema
    /// evolves tolerantly through the shared walker.
    struct SettingsSetting
    {
        /// @brief Stable setting identifier a resolver switches on and a store persists.
        string Id;
        /// @brief Display-only label shown in the settings UI.
        string Label;
        /// @brief Display-only long-form description shown in the settings UI.
        string Description;
        /// @brief Which of the two shapes this setting is; selects the meaningful fields below.
        SettingsSettingKind Kind = SettingsSettingKind::Discrete;

        /// @brief The closed option list (Discrete only); the user picks only from it.
        vector<SettingsOption> Options;
        /// @brief Index into Options chosen by default (Discrete only).
        u32 DefaultOption = 0;

        /// @brief Minimum value of the scalar range (Scalar only).
        f32 Min = 0.0f;
        /// @brief Maximum value of the scalar range (Scalar only).
        f32 Max = 1.0f;
        /// @brief Step the UI increments the scalar by (Scalar only); 0 for a continuous slider.
        f32 Step = 0.0f;
        /// @brief Value chosen by default, within [Min, Max] (Scalar only).
        f32 DefaultValue = 0.0f;
    };

    /// @brief A named group of settings, presented together in the settings UI.
    struct SettingsCategory
    {
        /// @brief Stable category identifier, unique within the schema.
        string Id;
        /// @brief Display-only label shown as the group heading.
        string Label;
        /// @brief The settings this category groups, in presentation order.
        vector<SettingsSetting> Settings;
    };

    /// @brief One entry of a preset: the value it sets for a single preset-eligible setting.
    ///
    /// SettingId is either a schema setting id or the reserved GraphicsRenderScaleBuiltinId. For a
    /// discrete setting OptionId names one of its options; for a scalar setting (or render scale)
    /// ScalarValue carries the value.
    struct SettingsPresetEntry
    {
        /// @brief The setting this entry sets: a schema setting id, or the render-scale built-in id.
        string SettingId;
        /// @brief The chosen option id (discrete settings only); empty for a scalar entry.
        string OptionId;
        /// @brief The chosen scalar value (scalar settings and render scale only).
        f32 ScalarValue = 0.0f;
    };

    /// @brief A named value-set over the preset-eligible settings — Low / Medium / High / Ultra.
    ///
    /// A preset bundles quality, not identity: it names a value for each preset-eligible setting
    /// (every schema setting, plus the render-scale built-in), never the display/output-identity
    /// built-ins. "Custom" is the state when a value diverges from every preset, so it is not itself
    /// a preset. Presets are optional; a schema may declare none.
    struct SettingsPreset
    {
        /// @brief Stable preset identifier, unique within the schema.
        string Id;
        /// @brief Display-only label shown in the preset selector.
        string Label;
        /// @brief The per-setting values this preset applies.
        vector<SettingsPresetEntry> Entries;
    };

    /// @brief The reflected on-disk payload of a settings schema: its categories and presets.
    ///
    /// The single reflected record the cook writes and the loader reads through the shared
    /// WriteFields/ReadFields encoder, so a settings schema needs no bespoke binary format. A new
    /// category/setting/preset field evolves tolerantly within the fixed CookedSettingsSchemaVersion.
    struct SettingsSchemaData
    {
        /// @brief The setting categories, in presentation order.
        vector<SettingsCategory> Categories;
        /// @brief The presets, in presentation order; may be empty.
        vector<SettingsPreset> Presets;
        /// @brief Id of the preset applied first-run and on Reset; names one of Presets, or empty.
        string DefaultPreset;
    };

    /// @brief Cached, immutable cooked settings-schema asset: a domain's data-driven settings
    ///        declaration.
    ///
    /// A SettingsSchema carries only *structure* — a setting has options and a label — never the
    /// meaning of any option, which is the consuming game's and is resolved in code. It is read once
    /// to build a settings menu and to bound the persisted user choices. A CPU-only asset with no
    /// GPU resource; load it through AssetManager::Load by AssetId like any other asset.
    class SettingsSchema
    {
    public:
        /// @brief Creates a schema from its decoded record.
        /// @param data  The decoded categories, presets, and default-preset id.
        /// @return The constructed schema.
        static Ref<SettingsSchema> Create(SettingsSchemaData data);

        /// @brief Returns the setting categories, in presentation order.
        [[nodiscard]] std::span<const SettingsCategory> GetCategories() const
        {
            return m_Data.Categories;
        }

        /// @brief Returns the presets, in presentation order.
        [[nodiscard]] std::span<const SettingsPreset> GetPresets() const { return m_Data.Presets; }

        /// @brief Returns the id of the preset applied first-run and on Reset; empty when none.
        [[nodiscard]] const string& GetDefaultPreset() const { return m_Data.DefaultPreset; }

        /// @brief Finds a category by id.
        /// @param id  The category id to look up.
        /// @return The category, or nullptr when the schema declares no such category.
        [[nodiscard]] const SettingsCategory* FindCategory(std::string_view id) const;

        /// @brief Finds a setting by id across every category.
        /// @param id  The setting id to look up.
        /// @return The setting, or nullptr when no category declares it.
        [[nodiscard]] const SettingsSetting* FindSetting(std::string_view id) const;

        /// @brief Finds a preset by id.
        /// @param id  The preset id to look up.
        /// @return The preset, or nullptr when the schema declares no such preset.
        [[nodiscard]] const SettingsPreset* FindPreset(std::string_view id) const;

    private:
        explicit SettingsSchema(SettingsSchemaData data);

        /// @brief The decoded schema record.
        SettingsSchemaData m_Data;
    };

    /// @brief AssetTypeTrait specialization mapping SettingsSchema to AssetTypes::SettingsSchema.
    template <>
    struct AssetTypeTrait<SettingsSchema>
    {
        /// @brief The asset type tag for SettingsSchema.
        static constexpr AssetTypeId Type = AssetTypes::SettingsSchema;
    };
}

VE_ENUM(::Veng::SettingsSettingKind, 0x09F953567E757FB2ULL)
VE_ENUMERATOR(Discrete)
VE_ENUMERATOR(Scalar)
VE_ENUM_END();

VE_REFLECT(::Veng::SettingsOption, 0xBE912F1B3EE27C4AULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Label, .DisplayName = "Label")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsSetting, 0x0703F5293E8B531EULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Label, .DisplayName = "Label")
VE_FIELD(Description, .DisplayName = "Description")
VE_FIELD(Kind, .DisplayName = "Kind")
VE_ARRAY_FIELD(Options, .DisplayName = "Options")
VE_FIELD(DefaultOption, .DisplayName = "Default Option")
VE_FIELD(Min, .DisplayName = "Min")
VE_FIELD(Max, .DisplayName = "Max")
VE_FIELD(Step, .DisplayName = "Step")
VE_FIELD(DefaultValue, .DisplayName = "Default Value")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsCategory, 0x25AF3AEED7EF2135ULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Label, .DisplayName = "Label")
VE_ARRAY_FIELD(Settings, .DisplayName = "Settings")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsPresetEntry, 0x514ADC3F1ADF4D08ULL)
VE_FIELD(SettingId, .DisplayName = "Setting Id")
VE_FIELD(OptionId, .DisplayName = "Option Id")
VE_FIELD(ScalarValue, .DisplayName = "Scalar Value")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsPreset, 0x9255ADA1913AB5CEULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Label, .DisplayName = "Label")
VE_ARRAY_FIELD(Entries, .DisplayName = "Entries")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsSchemaData, 0x25E7448BBAAFD1D5ULL)
VE_ARRAY_FIELD(Categories, .DisplayName = "Categories")
VE_ARRAY_FIELD(Presets, .DisplayName = "Presets")
VE_FIELD(DefaultPreset, .DisplayName = "Default Preset")
VE_REFLECT_END();
