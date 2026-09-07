#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Render/DisplayModes.h>
#include <Veng/Render/GraphicsSchema.h>

namespace Veng
{
    class TypeRegistry;

    /// @brief The engine's own built-in display/output selections, persisted per machine.
    ///
    /// These are the settings the engine defines and enumerates from the hardware itself
    /// (resolution, fullscreen mode, monitor, refresh, present mode, frame cap, render scale, and a
    /// brightness/gamma output knob) — never authored in a game's schema. A preset may set only the
    /// render scale; the remaining display-identity fields are never touched by a preset. The
    /// defaults are the engine's neutral output: native resolution and refresh (a zero standing for
    /// "the display's own"), a windowed vsync'd surface, no frame cap, and no scale or
    /// brightness/gamma adjustment.
    struct BuiltinDisplayChoices
    {
        /// @brief Chosen window/render resolution in pixels; (0, 0) means the display's native size.
        uvec2 Resolution{0, 0};
        /// @brief Whether the window is windowed, borderless, or exclusive full-screen.
        FullscreenMode Fullscreen = FullscreenMode::Windowed;
        /// @brief Index of the monitor the window targets; 0 is the primary display.
        u32 MonitorId = 0;
        /// @brief Chosen refresh rate in Hz; 0 means the display's native refresh.
        u32 RefreshRateHz = 0;
        /// @brief How presented frames synchronize with the display.
        PresentMode Present = PresentMode::Vsync;
        /// @brief Upper bound on presented frames per second; 0 means uncapped.
        u32 FrameCapHz = 0;
        /// @brief Resolution multiplier the renderer draws at before upscaling to the window; 1 is native.
        f32 RenderScale = 1.0f;
        /// @brief Output brightness multiplier; 1 is no adjustment.
        f32 Brightness = 1.0f;
        /// @brief Output gamma adjustment; 1 is no adjustment.
        f32 Gamma = 1.0f;
    };

    /// @brief The on-disk format version of a persisted GraphicsChoices document.
    ///
    /// Written into every saved file and read back to drive tolerant migration: an older document
    /// still loads, its added settings gaining their schema defaults and its stale setting ids
    /// dropped. Bumped only when the migration a load performs must change.
    inline constexpr u32 GraphicsChoicesVersion = 1;

    /// @brief One persisted value for a single schema setting, keyed by setting id.
    ///
    /// The stored analogue of a GraphicsPresetEntry (a chosen value vs. a preset's declared one):
    /// for a discrete setting OptionId names the chosen option, for a scalar setting ScalarValue
    /// carries it. Keyed by SettingId rather than by index, so reordering the schema never silently
    /// repoints a saved choice.
    struct GraphicsChoice
    {
        /// @brief The schema setting this value is for.
        string SettingId;
        /// @brief The chosen option id (discrete settings only); empty for a scalar choice.
        string OptionId;
        /// @brief The chosen scalar value (scalar settings only).
        f32 ScalarValue = 0.0f;
    };

    /// @brief The complete persisted graphics preferences of one machine.
    ///
    /// The reflected document GraphicsSettings saves and loads: the format version, the last-applied
    /// preset id (a display hint; the live Custom query is authoritative), the per-setting chosen
    /// values, and the engine's built-in display selections. It serializes through the shared
    /// reflection/JSON walker, so a new field evolves tolerantly within the version.
    struct GraphicsChoices
    {
        /// @brief The on-disk format version; stamped to GraphicsChoicesVersion on save.
        u32 Version = GraphicsChoicesVersion;
        /// @brief Id of the last preset applied; empty when no preset has been applied.
        string ActivePreset;
        /// @brief The persisted per-setting values, keyed by setting id.
        vector<GraphicsChoice> Choices;
        /// @brief The engine built-in display/output selections.
        BuiltinDisplayChoices Display;
    };

    /// @brief Construction parameters for GraphicsSettings.
    struct GraphicsSettingsInfo
    {
        /// @brief The loaded schema bounding the choices; null means no schema (display group only).
        ///
        /// Borrowed and must outlive the service. The engine holds it resident through an asset
        /// handle; a test owns it through a Ref for its lifetime.
        const GraphicsSchema* Schema = nullptr;
        /// @brief Registry the JSON walker resolves the choices' reflected types through.
        ///
        /// Borrowed and must outlive the service. Required for Load/Save; the settings types must be
        /// registered in it (RegisterBuiltinTypes does so).
        const TypeRegistry* Types = nullptr;
        /// @brief Absolute path of the per-machine settings file; empty disables persistence.
        ///
        /// When empty, Load yields defaults and Save is a no-op with a warning — the same
        /// fall-back-to-nothing posture the ImGui layout file takes when no config dir resolves.
        path ConfigPath;
    };

    /// @brief The per-machine graphics-preferences store: the loaded schema, the chosen values, and
    ///        the engine's built-in display selections, persisted as versioned JSON.
    ///
    /// Holds the state the settings menu reads and writes and the boot path applies. It carries ids
    /// and values, never their meaning — the schema bounds the choices; the resolver (game code)
    /// gives them effect. Persistence is per machine/install (a file under the user config
    /// directory), deliberately not the per-account save store: graphics config is a property of
    /// the install, and a save must not carry it.
    ///
    /// The store is standalone: it takes a schema, a type registry, and a config path, so it is
    /// exercised without an Application. Load is tolerant (a missing file yields defaults, an older
    /// version migrates, a corrupt file falls back to defaults with a warning and never aborts), and
    /// Save is atomic (a temporary renamed into place, so a crash mid-write leaves the previous file
    /// intact).
    class GraphicsSettings
    {
    public:
        /// @brief Constructs the store; the current choices start at the schema/engine defaults.
        /// @param info  The schema, type registry, and config path.
        explicit GraphicsSettings(GraphicsSettingsInfo info);

        /// @brief Returns the bounding schema, or null when none was supplied.
        [[nodiscard]] const GraphicsSchema* GetSchema() const { return m_Schema; }

        /// @brief Returns the complete current choices.
        [[nodiscard]] const GraphicsChoices& GetChoices() const { return m_Choices; }

        /// @brief Returns the current built-in display selections.
        [[nodiscard]] const BuiltinDisplayChoices& GetDisplay() const { return m_Choices.Display; }

        /// @brief Returns the current built-in display selections for in-place editing by the menu.
        [[nodiscard]] BuiltinDisplayChoices& GetDisplay() { return m_Choices.Display; }

        /// @brief Returns the id of the last preset applied; empty when none has been.
        [[nodiscard]] const string& GetActivePreset() const { return m_Choices.ActivePreset; }

        /// @brief Loads the choices from the config file, or leaves defaults when it cannot.
        ///
        /// A missing file yields the schema/engine defaults (the first-run path); a present file at
        /// an older version migrates tolerantly (added settings read their schema default, stale
        /// setting ids are dropped); an unreadable or corrupt file falls back to defaults and logs a
        /// warning. The store is always left usable.
        /// @return Empty on a clean load or a normal missing-file default; an error (state still
        ///         valid defaults) when the file was present but could not be read or parsed.
        VoidResult Load();

        /// @brief Writes the current choices to the config file atomically.
        ///
        /// The document is stamped with the current version and written through a temporary renamed
        /// into place, so a crash mid-write never truncates the file. Called by the menu on Apply,
        /// never per frame.
        /// @return Empty on success; an error when no config path is set or the write failed.
        [[nodiscard]] VoidResult Save() const;

        /// @brief Applies a named preset to the preset-eligible settings.
        ///
        /// Every schema setting the preset names gets a chosen value, and the reserved render-scale
        /// entry routes to the render-scale display built-in. The display-identity built-ins
        /// (resolution, monitor, refresh, fullscreen, present mode, frame cap, brightness/gamma) are
        /// never touched. ActivePreset is set to the applied preset.
        /// @param presetId  The preset to apply.
        /// @return Empty on success; an error when there is no schema or no such preset.
        VoidResult ApplyPreset(std::string_view presetId);

        /// @brief Resets every setting to its default and the display built-ins to engine defaults.
        ///
        /// Applies the schema's default preset to the preset-eligible settings — so ActivePreset
        /// becomes the default preset and the Custom query does not immediately fire — and resets
        /// the display-identity built-ins to their engine defaults (native resolution and refresh,
        /// windowed, vsync, no cap, no scale or brightness/gamma adjustment).
        void ResetToDefaults();

        /// @brief Returns the id of a preset the current choices match exactly, if any.
        ///
        /// A preset matches when every entry it declares equals the current chosen value (a schema
        /// setting's option/scalar, or the render-scale built-in). Returns the first such preset, or
        /// nullopt when the choices match none — the "Custom" state.
        [[nodiscard]] optional<string> MatchingPreset() const;

        /// @brief Returns whether the current choices match no preset — the "Custom" state.
        [[nodiscard]] bool IsCustom() const { return !MatchingPreset().has_value(); }

        /// @brief Returns the chosen option id for a discrete setting, or its schema default.
        /// @param settingId  The setting to read.
        /// @return The chosen option id, the setting's default option id when unset, or empty when
        ///         no schema declares the setting.
        [[nodiscard]] string GetChosenOption(std::string_view settingId) const;

        /// @brief Returns the chosen scalar value for a scalar setting, or its schema default.
        /// @param settingId  The setting to read.
        /// @return The chosen scalar, the setting's default value when unset, or 0 when no schema
        ///         declares the setting.
        [[nodiscard]] f32 GetChosenScalar(std::string_view settingId) const;

        /// @brief Sets the chosen option id for a discrete setting.
        /// @param settingId  The setting to write.
        /// @param optionId   The chosen option id.
        void SetChosenOption(std::string_view settingId, std::string_view optionId);

        /// @brief Sets the chosen scalar value for a scalar setting.
        /// @param settingId  The setting to write.
        /// @param value      The chosen scalar value.
        void SetChosenScalar(std::string_view settingId, f32 value);

    private:
        /// @brief Finds the stored choice for a setting id, or null when unset.
        [[nodiscard]] const GraphicsChoice* FindChoice(std::string_view settingId) const;

        /// @brief Inserts or updates the stored choice for a setting id.
        void SetChoice(std::string_view settingId, std::string_view optionId, f32 scalarValue);

        /// @brief Drops stored choices for setting ids the schema no longer declares.
        void DropStaleChoices();

        /// @brief Returns whether the current choices match the given preset exactly.
        [[nodiscard]] bool PresetMatches(const GraphicsPreset& preset) const;

        /// @brief The bounding schema (borrowed, nullable).
        const GraphicsSchema* m_Schema = nullptr;
        /// @brief The registry the JSON walker resolves the choices' types through (borrowed).
        const TypeRegistry* m_Types = nullptr;
        /// @brief The per-machine settings file; empty disables persistence.
        path m_ConfigPath;
        /// @brief The current choices.
        GraphicsChoices m_Choices;
    };
}

VE_REFLECT(::Veng::BuiltinDisplayChoices, 0xABF64230971FF76AULL)
VE_FIELD(Resolution, .DisplayName = "Resolution")
VE_FIELD(Fullscreen, .DisplayName = "Fullscreen")
VE_FIELD(MonitorId, .DisplayName = "Monitor Id")
VE_FIELD(RefreshRateHz, .DisplayName = "Refresh Rate Hz")
VE_FIELD(Present, .DisplayName = "Present Mode")
VE_FIELD(FrameCapHz, .DisplayName = "Frame Cap Hz")
VE_FIELD(RenderScale, .DisplayName = "Render Scale")
VE_FIELD(Brightness, .DisplayName = "Brightness")
VE_FIELD(Gamma, .DisplayName = "Gamma")
VE_REFLECT_END();

VE_REFLECT(::Veng::GraphicsChoice, 0x13CA5388C2FC3B58ULL)
VE_FIELD(SettingId, .DisplayName = "Setting Id")
VE_FIELD(OptionId, .DisplayName = "Option Id")
VE_FIELD(ScalarValue, .DisplayName = "Scalar Value")
VE_REFLECT_END();

VE_REFLECT(::Veng::GraphicsChoices, 0x82D18C94E0BA3680ULL)
VE_FIELD(Version, .DisplayName = "Version")
VE_FIELD(ActivePreset, .DisplayName = "Active Preset")
VE_ARRAY_FIELD(Choices, .DisplayName = "Choices")
VE_FIELD(Display, .DisplayName = "Display")
VE_REFLECT_END();
