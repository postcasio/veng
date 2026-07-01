#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/UI/Types.h>

namespace VengEditor
{
    /// @brief Strips the leading unsaved-changes marker ('*') from a panel title.
    ///
    /// A document panel's display title carries a '*' while it has unsaved changes, so the raw
    /// title is not a stable address — an edit would rename the panel out from under a caller
    /// holding its name. Every by-title lookup (the MCP tools, the host's panel resolution)
    /// compares marker-stripped titles, and the listing tools report the stripped form, so a
    /// panel's externally visible name is stable across the dirty toggle.
    /// @param title  A panel display title, possibly carrying the marker.
    /// @return The title without the marker — the stable panel address.
    [[nodiscard]] inline Veng::string_view StripUnsavedMarker(Veng::string_view title)
    {
        return title.starts_with('*') ? title.substr(1) : title;
    }

    /// @brief One reflected object a panel edits, named for external addressing.
    ///
    /// A panel hands these back through GetInspectables() so the generic editor MCP tools
    /// can read and write the same reflected models the panel already draws through
    /// DrawFieldWidget — no second API surface to keep in sync. The pointer must stay valid
    /// for the frame the tool runs in (the same guarantee a DrawFieldWidget edit relies on);
    /// a panel that rebuilds its model each frame returns the current pointer each call.
    struct Inspectable
    {
        /// @brief The addressing name the tools reference this object by (e.g. "renderSettings").
        Veng::string Name;

        /// @brief The reflected type of Data, resolved against the type registry to walk its fields.
        Veng::TypeId Type = Veng::InvalidTypeId;

        /// @brief Pointer to the reflected object; valid for the frame this is returned in.
        void* Data = nullptr;
    };

    /// @brief Base class for a dockable editor window.
    ///
    /// The host owns the open/close toggle, the dock id, and the Window-menu
    /// wiring. A panel provides its title and draws its body each frame inside
    /// the host-managed UI::Window scope. A render-owning panel holds a registered
    /// Veng::Renderer::Viewport; the engine drive-list renders it each frame, so the
    /// panel records no scene render of its own.
    class EditorPanel
    {
    public:
        virtual ~EditorPanel() = default;

        /// @brief Returns the panel's window title (stable across frames).
        [[nodiscard]] virtual Veng::string_view GetTitle() const = 0;

        /// @brief The reflected models this panel exposes to inspection/editing. Default empty.
        ///
        /// A panel that edits reflected objects through the inspector returns them here (the
        /// 1–3 objects it already holds), so the generic editor tools can walk them with the
        /// same reflection the UI uses. A panel with no such surface returns an empty list.
        /// @return The named reflected objects, each valid for the current frame.
        [[nodiscard]] virtual Veng::vector<Inspectable> GetInspectables() { return {}; }

        /// @brief Called after an external write into one of this panel's inspectables. Default no-op.
        ///
        /// The tools call this once the write lands so the panel runs its existing apply path —
        /// the same reaction a UI edit triggers (recook, mark dirty, live preview push, re-resolve).
        /// @param name  The Inspectable::Name that was written; empty or unknown names are ignored.
        virtual void OnInspectableChanged(Veng::string_view name) { (void)name; }

        /// @brief Returns additional ImGui window flags for this panel.
        [[nodiscard]] virtual Veng::UI::WindowFlags GetWindowFlags() const
        {
            return Veng::UI::WindowFlags::None;
        }

        /// @brief Draws the panel's ImGui contents into the current frame.
        virtual void OnUI() = 0;

        /// @brief Submits this panel's top-level window(s) for the frame.
        ///
        /// The host calls this once per open panel. The default wraps OnUI in a
        /// single UI::Window titled GetTitle() (with the panel's GetWindowFlags() and
        /// edge-to-edge padding for a NoScrollbar panel). An asset editor that hosts a
        /// private dockspace overrides this to submit its document window and the
        /// class-tagged child windows that dock into it.
        /// @param open  Host-owned visibility flag, toggled by the window close button.
        virtual void Draw(bool* open);
    };
}
