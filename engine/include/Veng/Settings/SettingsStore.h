#pragma once

#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Settings/SettingsSchema.h>

namespace Veng
{
    class TypeRegistry;

    /// @brief One persisted value for a single schema setting, keyed by setting id.
    ///
    /// The stored analogue of a SettingsPresetEntry (a chosen value vs. a preset's declared one):
    /// for a discrete setting OptionId names the chosen option, for a scalar setting ScalarValue
    /// carries it. Keyed by SettingId rather than by index, so reordering the schema never silently
    /// repoints a saved choice.
    struct SettingsChoice
    {
        /// @brief The schema setting this value is for.
        string SettingId;
        /// @brief The chosen option id (discrete settings only); empty for a scalar choice.
        string OptionId;
        /// @brief The chosen scalar value (scalar settings only).
        f32 ScalarValue = 0.0f;
    };

    /// @brief The on-disk format version of a persisted SettingsChoices document.
    ///
    /// Written into every saved file and read back to drive tolerant migration: an older document
    /// still loads, its added settings gaining their schema defaults and its stale setting ids
    /// dropped. Bumped only when the migration a load performs must change.
    inline constexpr u32 SettingsChoicesVersion = 1;

    /// @brief The domain-neutral persisted document: version, active preset, and per-setting values.
    ///
    /// The generic reflected document a SettingsStore saves and loads for a domain that needs no
    /// extra per-machine state (an audio store, say). A domain with its own built-ins (graphics'
    /// display selections) uses its own document type carrying these three fields plus its extras;
    /// the store is templated over the document type so any such document serializes flat through
    /// the shared reflection/JSON walker.
    struct SettingsChoices
    {
        /// @brief The on-disk format version; stamped to SettingsChoicesVersion on save.
        u32 Version = SettingsChoicesVersion;
        /// @brief Id of the last preset applied; empty when no preset has been applied.
        string ActivePreset;
        /// @brief The persisted per-setting values, keyed by setting id.
        vector<SettingsChoice> Choices;
    };

    /// @brief Construction parameters for a SettingsStore.
    struct SettingsStoreInfo
    {
        /// @brief The loaded schema bounding the choices; null means no schema.
        ///
        /// Borrowed and must outlive the store. The engine holds it resident through an asset
        /// handle; a test owns it through a Ref for its lifetime.
        const SettingsSchema* Schema = nullptr;
        /// @brief Registry the JSON walker resolves the document's reflected types through.
        ///
        /// Borrowed and must outlive the store. Required for Load/Save; the document type must be
        /// registered in it (RegisterBuiltinTypes registers the builtins).
        const TypeRegistry* Types = nullptr;
        /// @brief Absolute path of the per-machine settings file; empty disables persistence.
        ///
        /// When empty, Load yields defaults and Save is a no-op with a warning — the same
        /// fall-back-to-nothing posture the ImGui layout file takes when no config dir resolves.
        /// The filename distinguishes one domain's file from another's (graphics.json / audio.json).
        path ConfigPath;
    };

    /// @brief The document-type-independent heart of a per-machine settings store.
    ///
    /// Owns the schema pointer, the config path, and the tolerant-load / atomic-save / migrate /
    /// preset-apply / reset / Custom-detection logic. It reaches its document's domain-neutral trio
    /// (version, active preset, per-setting choices) and the whole reflected document through a
    /// small set of accessors the templated SettingsStore implements — so this shared logic is
    /// compiled once, JSON parsing stays out of the header, and it is oblivious to what a setting
    /// *means* (the schema bounds the choices; a resolver, in game code, gives them effect).
    ///
    /// Persistence is per machine/install (a file under the user config directory), deliberately
    /// not the per-account save store. Load is tolerant (a missing file yields defaults, an older
    /// version migrates, a corrupt file falls back to defaults with a warning and never aborts) and
    /// Save is atomic (a temporary renamed into place, so a crash mid-write leaves the previous file
    /// intact).
    class SettingsStoreBase
    {
    public:
        virtual ~SettingsStoreBase() = default;

        SettingsStoreBase(const SettingsStoreBase&) = delete;
        SettingsStoreBase& operator=(const SettingsStoreBase&) = delete;
        SettingsStoreBase(SettingsStoreBase&&) = delete;
        SettingsStoreBase& operator=(SettingsStoreBase&&) = delete;

        /// @brief Returns the bounding schema, or null when none was supplied.
        [[nodiscard]] const SettingsSchema* GetSchema() const { return m_Schema; }

        /// @brief Returns the id of the last preset applied; empty when none has been.
        [[nodiscard]] const string& GetActivePreset() const { return AccessActivePreset(); }

        /// @brief Whether the last Load() found an existing config file on disk.
        ///
        /// True when a settings file was present at the config path (a returning install), even if
        /// it was then unreadable or malformed and the store fell back to defaults; false after the
        /// first-run missing-file default, when no config path is set, and before any Load(). Lets a
        /// consumer tell a first launch from a returning one without re-deriving the engine's
        /// private config path itself.
        [[nodiscard]] bool WasLoadedFromFile() const { return m_LoadedFromFile; }

        /// @brief Loads the choices from the config file, or leaves defaults when it cannot.
        ///
        /// A missing file yields the schema defaults (the first-run path); a present file at an
        /// older version migrates tolerantly (added settings read their schema default, stale
        /// setting ids are dropped); an unreadable or corrupt file falls back to defaults and logs a
        /// warning. The store is always left usable.
        /// @return Empty on a clean load or a normal missing-file default; an error (state still
        ///         valid defaults) when the file was present but could not be read or parsed.
        VoidResult Load();

        /// @brief Writes the current choices to the config file atomically.
        ///
        /// The document is written through a temporary renamed into place, so a crash mid-write
        /// never truncates the file. Called by a menu on Apply, never per frame.
        /// @return Empty on success; an error when no config path is set or the write failed.
        [[nodiscard]] VoidResult Save() const;

        /// @brief Applies a named preset to the preset-eligible settings.
        ///
        /// Every schema setting the preset names gets a chosen value; an entry naming a domain
        /// built-in routes through the domain's ApplyBuiltinPresetEntry hook. ActivePreset is set to
        /// the applied preset.
        /// @param presetId  The preset to apply.
        /// @return Empty on success; an error when there is no schema or no such preset.
        VoidResult ApplyPreset(std::string_view presetId);

        /// @brief Resets every setting to its default and any domain built-ins to their defaults.
        ///
        /// Clears the choices and active preset, resets the domain built-ins (ResetBuiltinsToDefaults
        /// hook), then applies the schema's default preset when it declares one — so with a default
        /// preset ActivePreset becomes it and the Custom query does not immediately fire, and with
        /// none each setting simply reads its own schema default.
        void ResetToDefaults();

        /// @brief Returns the id of a preset the current choices match exactly, if any.
        ///
        /// A preset matches when every entry it declares equals the current chosen value (a schema
        /// setting's option/scalar, or a domain built-in via MatchBuiltinPresetEntry). Returns the
        /// first such preset, or nullopt when the choices match none — the "Custom" state, also the
        /// answer for a schema that declares no presets.
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

    protected:
        /// @brief Constructs the base from the borrowed schema, registry, and config path.
        explicit SettingsStoreBase(SettingsStoreInfo info)
            : m_Schema(info.Schema), m_Types(info.Types), m_ConfigPath(std::move(info.ConfigPath))
        {
        }

        /// @brief Returns the document's per-setting choices for mutation.
        [[nodiscard]] virtual vector<SettingsChoice>& AccessChoices() = 0;
        /// @brief Returns the document's per-setting choices.
        [[nodiscard]] virtual const vector<SettingsChoice>& AccessChoices() const = 0;
        /// @brief Returns the document's active-preset id for mutation.
        [[nodiscard]] virtual string& AccessActivePreset() = 0;
        /// @brief Returns the document's active-preset id.
        [[nodiscard]] virtual const string& AccessActivePreset() const = 0;
        /// @brief Stamps the document's version field with the document type's current version.
        virtual void StampDocVersion() = 0;
        /// @brief Resets the whole document to its struct defaults (the overlay base for a load).
        virtual void SetDocToStructDefaults() = 0;
        /// @brief Returns a pointer to the whole reflected document, for the JSON walker.
        [[nodiscard]] virtual void* DocData() = 0;
        /// @brief Returns a const pointer to the whole reflected document, for the JSON walker.
        [[nodiscard]] virtual const void* DocData() const = 0;
        /// @brief Returns the reflection TypeId of the document type.
        [[nodiscard]] virtual TypeId DocTypeId() const = 0;

        /// @brief Applies a preset entry naming a domain built-in, if this domain owns it.
        ///
        /// Called for every preset entry whose SettingId is not a schema setting. The default
        /// recognizes none. The graphics domain overrides it to route the render-scale built-in.
        /// @param entry  The preset entry.
        /// @return True when this domain consumed the entry as a built-in; false to fall through to
        ///         storing it as an ordinary keyed choice.
        [[nodiscard]] virtual bool ApplyBuiltinPresetEntry(const SettingsPresetEntry& /*entry*/)
        {
            return false;
        }

        /// @brief Whether a domain built-in preset entry matches the current value.
        ///
        /// @param entry  The preset entry.
        /// @return nullopt when this entry names no built-in this domain owns; otherwise whether the
        ///         current built-in value equals the entry's.
        [[nodiscard]] virtual optional<bool>
        MatchBuiltinPresetEntry(const SettingsPresetEntry& /*entry*/) const
        {
            return std::nullopt;
        }

        /// @brief Resets the domain's own built-ins to their defaults, called from ResetToDefaults.
        virtual void ResetBuiltinsToDefaults() {}

        /// @brief Whether two scalar values match within the store's preset-comparison tolerance.
        ///
        /// A scalar survives a JSON text round-trip with tiny rounding, so a preset match compares
        /// within a small epsilon rather than bit-exact.
        [[nodiscard]] static bool ScalarsMatch(f32 a, f32 b);

        /// @brief The bounding schema (borrowed, nullable).
        const SettingsSchema* m_Schema = nullptr;
        /// @brief The registry the JSON walker resolves the document's types through (borrowed).
        const TypeRegistry* m_Types = nullptr;
        /// @brief The per-machine settings file; empty disables persistence.
        path m_ConfigPath;
        /// @brief Whether the last Load() found an existing config file (see WasLoadedFromFile).
        bool m_LoadedFromFile = false;

    private:
        /// @brief Finds the stored choice for a setting id, or null when unset.
        [[nodiscard]] const SettingsChoice* FindChoice(std::string_view settingId) const;

        /// @brief Inserts or updates the stored choice for a setting id.
        void SetChoice(std::string_view settingId, std::string_view optionId, f32 scalarValue);

        /// @brief Drops stored choices for setting ids the schema no longer declares.
        void DropStaleChoices();

        /// @brief Returns whether the current choices match the given preset exactly.
        [[nodiscard]] bool PresetMatches(const SettingsPreset& preset) const;
    };

    /// @brief The per-machine settings store over a reflected document type.
    ///
    /// Adds the typed document to SettingsStoreBase's shared logic and serializes it flat through
    /// the shared JSON walker: every field is a top-level JSON key, so a document carrying extra
    /// fields beyond the domain-neutral trio (a display group, say) writes those as sibling keys and
    /// the file format is exactly the document's reflection. TDoc must expose the three
    /// domain-neutral members `u32 Version`, `string ActivePreset`, and `vector<SettingsChoice>
    /// Choices`, and be a registered reflected type.
    /// @tparam TDoc  The reflected document type this store persists.
    template <class TDoc>
    class SettingsStore : public SettingsStoreBase
    {
    public:
        /// @brief Constructs the store; the document starts at its struct defaults.
        /// @param info  The schema, type registry, and config path.
        explicit SettingsStore(SettingsStoreInfo info) : SettingsStoreBase(std::move(info)) {}

        /// @brief Returns the complete current document.
        [[nodiscard]] const TDoc& GetDocument() const { return m_Doc; }

    protected:
        /// @brief Returns the complete current document for in-place mutation by a domain façade.
        [[nodiscard]] TDoc& MutableDocument() { return m_Doc; }

        [[nodiscard]] vector<SettingsChoice>& AccessChoices() override { return m_Doc.Choices; }
        [[nodiscard]] const vector<SettingsChoice>& AccessChoices() const override
        {
            return m_Doc.Choices;
        }
        [[nodiscard]] string& AccessActivePreset() override { return m_Doc.ActivePreset; }
        [[nodiscard]] const string& AccessActivePreset() const override
        {
            return m_Doc.ActivePreset;
        }
        void StampDocVersion() override { m_Doc.Version = TDoc{}.Version; }
        void SetDocToStructDefaults() override { m_Doc = TDoc{}; }
        [[nodiscard]] void* DocData() override { return &m_Doc; }
        [[nodiscard]] const void* DocData() const override { return &m_Doc; }
        [[nodiscard]] TypeId DocTypeId() const override { return TypeIdOf<TDoc>(); }

    private:
        /// @brief The current document.
        TDoc m_Doc;
    };
}

VE_REFLECT(::Veng::SettingsChoice, 0x13CA5388C2FC3B58ULL)
VE_FIELD(SettingId, .DisplayName = "Setting Id")
VE_FIELD(OptionId, .DisplayName = "Option Id")
VE_FIELD(ScalarValue, .DisplayName = "Scalar Value")
VE_REFLECT_END();

VE_REFLECT(::Veng::SettingsChoices, 0xA50091AD6108F9D9ULL)
VE_FIELD(Version, .DisplayName = "Version")
VE_FIELD(ActivePreset, .DisplayName = "Active Preset")
VE_ARRAY_FIELD(Choices, .DisplayName = "Choices")
VE_REFLECT_END();
