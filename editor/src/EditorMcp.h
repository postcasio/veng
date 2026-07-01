#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Result.h>

namespace Veng
{
    class Scene;
    class TypeRegistry;

    namespace Renderer
    {
        class Viewport;
    }

    namespace Mcp
    {
        class McpServer;
        struct McpMutation;
    }
}

namespace VengEditor
{
    class AssetEditorPanel;
    class AssetSourceIndex;
    class EditorPanel;

    /// @brief The provider seam the generic editor tools reach the live editor state through.
    ///
    /// Mirrors Veng::Mcp::McpHost's posture: the editor host fills these closures so the tools,
    /// registered from the editor side, reach the panel set and the focused document without the
    /// MCP library knowing about any panel. Every closure runs on the render thread during
    /// McpServer::Pump(), so it may freely touch panel and scene state. The referenced systems
    /// must outlive the server.
    struct EditorMcpHost
    {
        /// @brief Resolves an Inspectable's TypeId to its reflected fields for the field walk.
        Veng::TypeRegistry& Types;

        /// @brief Returns the host's open panels, in Window-menu order.
        ///
        /// editor.list_panels reports these; editor.inspect / editor.set_field resolve a named
        /// panel among them by title. The returned pointers are valid for the current Pump().
        Veng::function<Veng::vector<EditorPanel*>()> Panels;

        /// @brief Returns the focused asset-editor document, or null when none holds focus.
        ///
        /// The lifecycle verbs (editor.save / editor.undo / editor.redo) dispatch to this — the
        /// same document the File/Edit menus target. Null makes those verbs report "no focused
        /// document", never a null deref.
        Veng::function<AssetEditorPanel*()> FocusedDocument;

        /// @brief Returns the document scene the mutation router edits, or null when none.
        ///
        /// The same scene McpHost::CurrentWorld serves; the command router reads a component's
        /// current bytes from it to build an undoable command. Null makes the router decline.
        Veng::function<Veng::Scene*()> DocumentScene;

        /// @brief Returns the project's AssetId→source index, or null when no project is open.
        ///
        /// editor.list_assets browses these entries (id, name, type, source path). Null (the
        /// no-project state) makes the tool report an empty asset list.
        Veng::function<const AssetSourceIndex*()> AssetSources;

        /// @brief Opens the registered editor for an asset, resolving its type from the source index.
        ///
        /// editor.open_asset dispatches to this — PanelHost::OpenAssetEditor plus the index type
        /// resolution. Returns false when the id is unknown to the project or its type has no
        /// registered editor, surfaced by the tool as a located error.
        Veng::function<bool(Veng::AssetId)> OpenAsset;

        /// @brief Shows or hides an open panel by title, or opens/closes a document panel.
        ///
        /// editor.set_panel_visible flips the panel's Open flag at the frame's panel-erase point.
        /// Returns whether a panel matched the title (false makes the tool report "no such panel").
        Veng::function<bool(Veng::string_view title, bool visible)> SetPanelVisible;

        /// @brief Resolves an open panel's Offscreen viewport by title, or null.
        ///
        /// editor.screenshot_panel captures this viewport through the shared render-thread Download
        /// path. Null when the title names no open panel or the panel renders no scene.
        Veng::function<Veng::Renderer::Viewport*(Veng::string_view title)> PanelViewport;

        /// @brief Kicks a cook of the asset off the render thread, returning immediately.
        ///
        /// editor.request_cook dispatches to this. It resolves the id's source through the index,
        /// builds a CookRequest, and kicks EditorHost::RequestCook — fire-and-poll: the cook's
        /// completion lands earlier in a later frame than McpServer::Pump(), so the tool must not
        /// block on it (that would deadlock the frame). editor.cook_status reports completion.
        /// Returns a located error when the id is unknown or cook-on-demand is unconfigured.
        Veng::function<Veng::VoidResult(Veng::AssetId)> RequestCook;

        /// @brief Reports the latest cook status of an asset, or nullopt when none was requested.
        ///
        /// editor.cook_status reads this: "running" while the cook is in flight, "ok" once it
        /// mounted, or an error string when it failed. nullopt means no cook of this id has been
        /// requested through editor.request_cook. Runs on the render thread, so it shares the
        /// render-thread state RequestCook's continuation updates with no locking.
        Veng::function<Veng::optional<Veng::string>(Veng::AssetId)> CookStatus;
    };

    /// @brief Registers the generic editor property/command tools into the server.
    ///
    /// Adds editor.list_panels, editor.inspect, editor.set_field, editor.save, editor.undo, and
    /// editor.redo — all fully generic over the panel Inspectable seam, with no per-panel code.
    /// editor.set_field walks the same ReflectToJson JsonToFields the inspector walks through
    /// DrawFieldWidget, then calls OnInspectableChanged so the panel recooks / marks dirty as a UI
    /// edit would. The save/undo/redo verbs ride the focused AssetEditorPanel's virtuals.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam, captured by reference into each handler; must outlive the server.
    void RegisterEditorReflectionTools(Veng::Mcp::McpServer& server, const EditorMcpHost& host);

    /// @brief Registers the editor's non-reflection host tools into the server.
    ///
    /// Adds editor.open_asset, editor.set_panel_visible, editor.list_assets (paginated over the
    /// AssetSourceIndex), editor.screenshot_panel (the shared viewport-capture path against a
    /// named panel), editor.request_cook (fire-and-poll cook-on-demand), and editor.cook_status.
    /// Each reaches the editor host through the EditorMcpHost closures; a null closure makes its
    /// tool report the feature unavailable rather than crash.
    /// @param server  The server to register the tools into (before its first Pump()).
    /// @param host    The provider seam, captured by reference into each handler; must outlive the server.
    void RegisterEditorHostTools(Veng::Mcp::McpServer& server, const EditorMcpHost& host);

    /// @brief Applies a world mutation as an undoable command against the focused document.
    ///
    /// The implementation of Veng::Mcp::McpHost::ApplyMutation an editor host installs: an
    /// AddComponent / RemoveComponent / SetField mutation is pushed onto the focused document's
    /// CommandStack (so an agent's edit undoes like a human's and marks the document dirty),
    /// returning true. A mutation the editor path does not route (spawn / destroy / load-prefab, or
    /// no focused document / no document scene) returns false, leaving the tool's raw path to apply
    /// it. The host wires this as `ApplyMutation = [&](const auto& m) { return ApplyEditorMutation(host, m); }`.
    /// @param host      The editor provider seam (focused document + document scene + registry).
    /// @param mutation  The resolved mutation to route.
    /// @return True when routed onto the command stack; false to fall back to the raw scene edit.
    bool ApplyEditorMutation(const EditorMcpHost& host, const Veng::Mcp::McpMutation& mutation);
}
