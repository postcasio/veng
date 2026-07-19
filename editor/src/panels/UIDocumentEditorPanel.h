#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/UIDocument.h>

#include <VengEditor/CookRequest.h>

#include "AssetEditorPanel.h"
#include "AssetSaveModel.h"

#include "panels/TextureEditorPanel.h" // CookDriver alias
#include "panels/UIDocumentSource.h"

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
    class AssetSourceIndex;

    /// @brief Docked authoring panel for a `*.vui.xml` UIDocument source.
    ///
    /// Hosts a WYSIWYG canvas — an Offscreen Veng::Renderer::Viewport rendering the live
    /// Gui::Document instantiated from the cooked recipe, sampled into an ImGui::Image — an
    /// element-tree outline over the instantiated tree, and a resolved-style inspector drawn
    /// through Veng::UI for the selected element.
    ///
    /// It writes nothing until an explicit save: an authoring action rewrites the in-memory markup
    /// and marks the document dirty, and Save writes the `*.vui.xml` and then recooks, hot-reloading
    /// behind its stable AssetHandle. The cook reads the file, so the canvas renders the last
    /// *saved* markup and an unsaved edit appears on save. Reverting reloads the file, which is also
    /// how a change made in an external editor is picked up.
    class UIDocumentEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the UIDocument at @p id / @p sourcePath.
        ///
        /// @param id          The UIDocument asset id, addressable behind the cook-on-demand mount.
        /// @param sourcePath  Absolute path to the `*.vui.xml` source.
        /// @param sources     Manifest source index backing the texture asset-chip picker.
        /// @param app         Application the canvas's Offscreen viewport is registered into.
        /// @param assets      Asset manager the recipe and its font/texture dependencies load through.
        /// @param imgui       ImGui layer the canvas output texture registers into.
        /// @param cook        Off-thread cook driver bound to the host's RequestCook.
        UIDocumentEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                              const AssetSourceIndex& sources, Veng::Application& app,
                              Veng::AssetManager& assets, Veng::ImGuiLayer& imgui, CookDriver cook);
        ~UIDocumentEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

        /// @brief Writes the edited markup back to the `*.vui.xml`, then recooks.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true while the markup holds edits not yet written to the source.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Dirty; }

    private:
        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Reloads the markup from disk, discarding unsaved edits, and recooks.
        ///
        /// Also the route by which an edit made in an external editor reaches the canvas.
        void ReloadSource();

        /// @brief Instantiates a fresh live document from the resident recipe and attaches it.
        ///
        /// Detaches and drops any prior document, instantiates the reloaded recipe (resolving its
        /// font through the asset manager), attaches it to the canvas viewport, and clears the
        /// selection. A no-op when the recipe is not yet resident.
        void RebuildDocument();

        /// @brief Draws the element-tree outline, one selectable row per element, and recurses.
        void DrawOutline(Veng::Gui::Element& element, Veng::u32& index);

        /// @brief Draws the selected element's resolved style through Veng::UI (read-only).
        ///
        /// For a selected Image element it also draws an editable texture asset-chip over the
        /// element's `src`, so a dropped or picked texture repoints the image and recooks.
        void DrawStyleInspector();

        /// @brief Rewrites the in-memory markup and marks the document dirty; writes no file.
        ///
        /// The single authoring mutation every edit action routes through. The source reaches disk
        /// only through Save.
        /// @param edit  Transforms the markup; returns nullopt to abandon the edit unchanged.
        void
        EditSource(const Veng::function<Veng::optional<Veng::string>(const Veng::string&)>& edit);

        /// @brief Appends a new `<Image>` child under the document root.
        ///
        /// The authored image starts un-sourced (a styled box); its texture is assigned through the
        /// inspector's asset-chip. This is the "create an Image in the tree" authoring entry.
        void AddImageElement();

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;
        const AssetSourceIndex& m_Sources;

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

        /// @brief The edited markup; the file's content only after a save.
        UIDocumentSource m_Source;

        /// @brief Serialises recooks; a save landing behind one in flight queues rather than drops.
        CookGate m_Gate;

        /// @brief Whether the markup holds edits not yet written to the source.
        bool m_Dirty = false;

        /// @brief The recook produced a fresh mount; the document rebuilds on the next OnUI frame.
        bool m_DocumentDirty = false;
        Veng::optional<Veng::string> m_CookError;
    };
}
