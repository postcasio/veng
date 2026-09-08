#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Render/DisplayModes.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Settings/SettingsStore.h>

namespace Veng
{
    class TypeRegistry;

    /// @brief The graphics-domain spelling of a persisted per-setting choice.
    ///
    /// An alias of the shared SettingsChoice — kept so graphics-domain code reads in graphics terms.
    using GraphicsChoice = SettingsChoice;

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
    /// Written into every saved file and read back to drive tolerant migration. Bumped only when the
    /// migration a load performs must change.
    inline constexpr u32 GraphicsChoicesVersion = 1;

    /// @brief The complete persisted graphics preferences of one machine.
    ///
    /// The reflected document GraphicsSettings saves and loads: the format version, the last-applied
    /// preset id (a display hint; the live Custom query is authoritative), the per-setting chosen
    /// values, and the engine's built-in display selections. It serializes flat through the shared
    /// reflection/JSON walker, so a new field evolves tolerantly within the version; the Display
    /// built-ins ride the same flat document with no sub-object of their own.
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

    /// @brief The per-machine graphics-preferences store: the graphics-domain façade over the
    ///        shared settings core, adding the engine's built-in display selections.
    ///
    /// A thin façade over SettingsStore<GraphicsChoices>: the core owns the schema, the tolerant
    /// versioned JSON persistence, and preset apply/reset/Custom detection, while this class adds
    /// the graphics-only display built-ins (which have no analogue in another settings domain) and
    /// routes the one preset-eligible built-in — the render scale — through the core's built-in
    /// hooks. It carries ids and values, never their meaning: the schema bounds the choices; a
    /// resolver (game code) gives them effect. Persistence is per machine/install, deliberately not
    /// the per-account save store: graphics config is a property of the install, and a save must
    /// not carry it.
    class GraphicsSettings : public SettingsStore<GraphicsChoices>
    {
    public:
        /// @brief Constructs the store; the current choices start at the schema/engine defaults.
        /// @param info  The schema, type registry, and config path.
        explicit GraphicsSettings(GraphicsSettingsInfo info)
            : SettingsStore<GraphicsChoices>(
                  SettingsStoreInfo{.Schema = info.Schema,
                                    .Types = info.Types,
                                    .ConfigPath = std::move(info.ConfigPath)})
        {
            // Run the reset now that the render-scale built-in hooks are live (a base-constructor
            // reset would route the render-scale preset entry as an ordinary choice).
            ResetToDefaults();
        }

        /// @brief Returns the complete current choices.
        [[nodiscard]] const GraphicsChoices& GetChoices() const { return GetDocument(); }

        /// @brief Returns the current built-in display selections.
        [[nodiscard]] const BuiltinDisplayChoices& GetDisplay() const
        {
            return GetDocument().Display;
        }

        /// @brief Returns the current built-in display selections for in-place editing by the menu.
        [[nodiscard]] BuiltinDisplayChoices& GetDisplay() { return MutableDocument().Display; }

    protected:
        /// @brief Routes the render-scale built-in preset entry to the display selections.
        [[nodiscard]] bool ApplyBuiltinPresetEntry(const SettingsPresetEntry& entry) override
        {
            if (entry.SettingId == GraphicsRenderScaleBuiltinId)
            {
                MutableDocument().Display.RenderScale = entry.ScalarValue;
                return true;
            }
            return false;
        }

        /// @brief Matches the render-scale built-in preset entry against the display selection.
        [[nodiscard]] optional<bool>
        MatchBuiltinPresetEntry(const SettingsPresetEntry& entry) const override
        {
            if (entry.SettingId == GraphicsRenderScaleBuiltinId)
            {
                return ScalarsMatch(GetDocument().Display.RenderScale, entry.ScalarValue);
            }
            return std::nullopt;
        }

        /// @brief Resets the display-identity built-ins to their engine defaults.
        void ResetBuiltinsToDefaults() override
        {
            MutableDocument().Display = BuiltinDisplayChoices{};
        }
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

VE_REFLECT(::Veng::GraphicsChoices, 0x82D18C94E0BA3680ULL)
VE_FIELD(Version, .DisplayName = "Version")
VE_FIELD(ActivePreset, .DisplayName = "Active Preset")
VE_ARRAY_FIELD(Choices, .DisplayName = "Choices")
VE_FIELD(Display, .DisplayName = "Display")
VE_REFLECT_END();
