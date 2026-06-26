#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionRole.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;

    namespace Renderer
    {
        class Context;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief Off-thread cook callback bound to EditorHost::RequestCook.
    ///
    /// The host shadow-mounts the cooked result and delivers a MountHandle (or
    /// error) back on the main thread.
    using CookDriver = Veng::function<void(const CookRequest&,
                                           Veng::function<void(Veng::Result<Veng::MountHandle>)>)>;

    /// @brief Reads the project's active build configuration (or null for the zero-config state).
    ///
    /// Read each frame so the resolved-format read-out tracks a configuration change made in
    /// the Project Settings panel while this editor is open.
    using ActiveConfigAccessor = Veng::function<const Veng::BuildConfiguration*()>;

    /// @brief Reads the host-clamped configuration the editor's live preview cooks through.
    ///
    /// Host-safe by default and never an unsamplable codec; "preview as ship config" substitutes
    /// a previewable ship configuration. Read each frame so flipping the preview selection
    /// re-cooks the open texture editor, and to label the preview with what it shows.
    using PreviewConfigAccessor = Veng::function<Veng::BuildConfiguration()>;

    /// @brief Docked panel for previewing and editing a .tex.json texture source.
    ///
    /// Shows the decoded texture in a live preview, exposes sampler and sRGB
    /// settings, recooks on change (debounced), and round-trips the JSON on save.
    class TextureEditorPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the editor for the texture at @p id / @p sourcePath.
        TextureEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                           Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, CookDriver cook,
                           ActiveConfigAccessor activeConfig, PreviewConfigAccessor previewConfig);
        ~TextureEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

    private:
        /// @brief Sampler filter modes; ordinals match the importer's string vocabulary.
        enum class Filter : Veng::u32
        {
            Nearest = 0,
            Linear = 1
        };
        /// @brief Texture wrap modes; ordinals match the importer's string vocabulary.
        enum class Wrap : Veng::u32
        {
            Repeat = 0,
            MirroredRepeat = 1,
            ClampToEdge = 2,
            ClampToBorder = 3
        };

        /// @brief Editable subset of the .tex.json fields.
        struct Settings
        {
            bool Srgb = true;
            Filter Min = Filter::Linear;
            Filter Mag = Filter::Linear;
            Filter Mipmap = Filter::Linear;
            Wrap WrapU = Wrap::Repeat;
            Wrap WrapV = Wrap::Repeat;
            /// @brief The authored compression role, or nullopt to fall back to the sRGB guess.
            Veng::optional<Veng::CompressionRole> Role;
        };

        /// @brief Reads the on-disk .tex.json into m_Settings; absent fields keep defaults.
        void LoadSettings();

        /// @brief Patches the settings keys in the existing JSON (preserving unknown keys)
        /// and writes it back with 4-space indent.
        /// @return False (error recorded) on I/O or parse failure.
        bool SaveSettings();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        CookDriver m_Cook;
        ActiveConfigAccessor m_ActiveConfig;
        PreviewConfigAccessor m_PreviewConfig;

        /// @brief Name of the preview configuration the last cook ran through; a change re-cooks.
        Veng::string m_PreviewConfigName;

        Settings m_Settings;

        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::ImGuiTexture> m_Preview;
        Veng::AssetHandle<Veng::Texture> m_Handle;
        Veng::MountHandle m_Mount;

        /// @brief Cook submitted but not yet mounted; suppresses concurrent cooks.
        bool m_Cooking = false;

        /// @brief A settings change is pending; fires TriggerCook when m_DebounceRemaining reaches zero.
        bool m_CookPending = false;
        Veng::f32 m_DebounceRemaining = 0.0f;

        /// @brief Handle is resident but the preview ImGuiTexture has not been (re)created;
        /// creation is deferred to OnUI where the ImGui frame is live.
        bool m_PreviewDirty = false;

        Veng::optional<Veng::string> m_CookError;
    };
}
