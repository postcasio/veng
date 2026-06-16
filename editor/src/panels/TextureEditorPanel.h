#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>

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
    // The cook driver the texture editor uses to recook its source on demand:
    // EditorHost::RequestCook bound to the host. The host shadow-mounts the
    // result and hands back a MountHandle (or an error) on the main thread.
    using CookDriver = Veng::function<void(
        const CookRequest&, Veng::function<void(Veng::Result<Veng::MountHandle>)>)>;

    // The texture editor: a docked panel showing a decoded texture in a live
    // preview, editing the .tex.json sampler/sRGB settings, recooking on change
    // (debounced), and round-tripping the JSON source on save. Opened by
    // double-clicking a texture in the asset browser.
    class TextureEditorPanel final : public EditorPanel
    {
    public:
        TextureEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                           Veng::Renderer::Context& context, Veng::AssetManager& assets,
                           Veng::ImGuiLayer& imgui, CookDriver cook);
        ~TextureEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnImGui() override;

    private:
        // Mirrors the .tex.json fields the editor exposes; each enum value matches
        // the importer's string vocabulary (TextureImporter.cpp).
        enum class Filter : Veng::u32 { Nearest = 0, Linear = 1 };
        enum class Wrap : Veng::u32 { Repeat = 0, MirroredRepeat = 1, ClampToEdge = 2, ClampToBorder = 3 };

        struct Settings
        {
            bool Srgb = true;
            Filter Min = Filter::Linear;
            Filter Mag = Filter::Linear;
            Filter Mipmap = Filter::Linear;
            Wrap WrapU = Wrap::Repeat;
            Wrap WrapV = Wrap::Repeat;
        };

        // Read the on-disk .tex.json into m_Settings (tolerant: absent fields keep
        // their defaults). Logs and keeps defaults on a malformed file.
        void LoadSettings();

        // Patch the settings keys in the existing JSON (preserving unknown keys)
        // and write it back with 4-space indent. Returns false (with an error
        // recorded) on an I/O or parse failure.
        bool SaveSettings();

        // Kick off a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        CookDriver m_Cook;

        Settings m_Settings;

        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::ImGuiTexture> m_Preview;
        Veng::AssetHandle<Veng::Texture> m_Handle;
        Veng::MountHandle m_Mount;

        // A recook is in flight (cook submitted, not yet mounted) — shows the
        // "Cooking…" overlay and suppresses a second concurrent cook.
        bool m_Cooking = false;

        // A settings change is pending a debounced cook: m_CookPending fires the
        // cook once m_DebounceRemaining counts down to zero.
        bool m_CookPending = false;
        Veng::f32 m_DebounceRemaining = 0.0f;

        // The texture handle is resident but its preview ImGuiTexture has not been
        // (re)created yet — deferred to OnImGui where the ImGui frame is live.
        bool m_PreviewDirty = false;

        Veng::optional<Veng::string> m_CookError;
    };
}
