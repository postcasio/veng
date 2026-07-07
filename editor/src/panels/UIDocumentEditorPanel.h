#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/UIDocument.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>

#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class Application;
    class AssetManager;
    class ImGuiLayer;
    class ImGuiTexture;
    class Scene;
    class TypeRegistry;

    namespace Gui
    {
        struct Element;
    }

    namespace Renderer
    {
        class Context;
        class Sampler;
        class Viewport;
    }
}

namespace VengEditor
{
    /// @brief Docked authoring panel for a `*.vui.xml` UIDocument source.
    ///
    /// Hosts a WYSIWYG canvas — an Offscreen Veng::Renderer::Viewport rendering the live
    /// Gui::Document instantiated from the cooked recipe, sampled into an ImGui::Image — an
    /// element-tree outline over the instantiated tree, and a resolved-style inspector drawn
    /// through Veng::UI for the selected element. Editing the markup (or opening the source in an
    /// external editor and saving) recooks off the render thread and hot-reloads the document
    /// behind its stable AssetHandle, so the canvas reflects the recook. The document is the
    /// source of truth; the panel edits it and observes the result.
    class UIDocumentEditorPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the editor for the UIDocument at @p id / @p sourcePath.
        ///
        /// @param id          The UIDocument asset id, addressable behind the cook-on-demand mount.
        /// @param sourcePath  Absolute path to the `*.vui.xml` source.
        /// @param app         Application the canvas's Offscreen viewport is registered into.
        /// @param assets      Asset manager the recipe and its font dependency load through.
        /// @param imgui       ImGui layer the canvas output texture registers into.
        /// @param cook        Off-thread cook driver bound to the host's RequestCook.
        UIDocumentEditorPanel(Veng::AssetId id, Veng::path sourcePath, Veng::Application& app,
                              Veng::AssetManager& assets, Veng::ImGuiLayer& imgui, CookDriver cook);
        ~UIDocumentEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

    private:
        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Instantiates a fresh live document from the resident recipe and attaches it.
        ///
        /// Detaches and drops any prior document, instantiates the reloaded recipe (resolving its
        /// font through the asset manager), attaches it to the canvas viewport, and clears the
        /// selection. A no-op when the recipe is not yet resident.
        void RebuildDocument();

        /// @brief Draws the element-tree outline, one selectable row per element, and recurses.
        void DrawOutline(Veng::Gui::Element& element, Veng::u32& index);

        /// @brief Draws the selected element's resolved style through Veng::UI (read-only).
        void DrawStyleInspector();

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;

        Veng::Application& m_App;
        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        CookDriver m_Cook;

        /// @brief The private type registry the canvas scene's builtin components register into.
        Veng::Unique<Veng::TypeRegistry> m_Types;
        /// @brief The empty canvas scene — the document composites over its cleared output.
        Veng::Unique<Veng::Scene> m_Scene;
        /// @brief The Offscreen viewport rendering the canvas; registered into the app drive-list.
        Veng::Unique<Veng::Renderer::Viewport> m_Viewport;
        /// @brief Edge-clamp sampler for the canvas output.
        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        /// @brief ImGuiTexture over the canvas viewport's output; re-created on resize/invalidory.
        Veng::Ref<Veng::ImGuiTexture> m_CanvasTexture;

        /// @brief The stable recipe handle; re-fetched behind each fresh cook-on-demand mount.
        Veng::AssetHandle<Veng::Gui::UIDocument> m_Handle;
        /// @brief The in-memory cook mount held behind the stable handle; replaced each recook.
        Veng::MountHandle m_Mount;

        /// @brief The live instantiated document hosted on the canvas viewport.
        Veng::Unique<Veng::Gui::Document> m_Document;
        /// @brief The pre-order index of the outline-selected element, or npos when none.
        Veng::usize m_Selected = static_cast<Veng::usize>(-1);
        /// @brief The selected element (parallel to m_Selected), or nullptr when none/rebuilt.
        Veng::Gui::Element* m_SelectedElement = nullptr;

        /// @brief Cook submitted but not yet mounted; suppresses concurrent cooks.
        bool m_Cooking = false;
        /// @brief The recook produced a fresh mount; the document rebuilds on the next OnUI frame.
        bool m_DocumentDirty = false;
        Veng::optional<Veng::string> m_CookError;
    };
}
