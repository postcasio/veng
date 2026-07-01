#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    class TypeRegistry;

    namespace Mcp
    {
        class McpServer;
        struct McpMutation;
    }
}

namespace VengEditor
{
    class AssetEditorPanel;
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
