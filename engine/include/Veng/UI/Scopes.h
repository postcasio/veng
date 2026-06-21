#pragma once
#include <Veng/Veng.h>
#include <Veng/UI/Types.h>

/// @brief RAII scope guards pairing each ImGui begin/push with its end/pop in the destructor.
///
/// A guard stores only a plain bool open-state — never an imgui handle — and its destructor
/// is defined out-of-line in Scopes.cpp, so this header names no imgui type. Every guard is
/// `[[nodiscard]]` with an explicit `operator bool`: `if (auto w = UI::Window("X")) { ... }`
/// draws the body only when open.
///
/// The begin/end asymmetry is baked into each guard so a call site cannot pair it wrong:
/// `Begin`/`BeginChild`/`BeginTable`'s end always runs, while
/// `BeginMenu`/`BeginMainMenuBar`/`BeginPopup`/`TreeNodeEx` pop only when the begin
/// returned true.

namespace Veng::UI
{
    /// @brief Scope guard for `ImGui::Begin`/`End`.
    ///
    /// `End` must run even when the window is collapsed, so the destructor calls it
    /// unconditionally; `m_Open` only gates the body via `operator bool`.
    class [[nodiscard]] ScopedWindow
    {
    public:
        /// @brief Constructs the guard with the result of a prior `Begin` call.
        /// @param open  Whether the window body should be drawn.
        explicit ScopedWindow(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::End` unconditionally.
        ~ScopedWindow();

        ScopedWindow(const ScopedWindow&) = delete;
        ScopedWindow& operator=(const ScopedWindow&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedWindow(ScopedWindow&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedWindow& operator=(ScopedWindow&&) = delete;

        /// @brief Returns true when the window body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the body is visible.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginChild`/`EndChild`.
    ///
    /// `EndChild` runs unconditionally, mirroring `Begin`/`End`.
    class [[nodiscard]] ScopedChild
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginChild` call.
        /// @param open  Whether the child region body should be drawn.
        explicit ScopedChild(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndChild` unconditionally.
        ~ScopedChild();

        ScopedChild(const ScopedChild&) = delete;
        ScopedChild& operator=(const ScopedChild&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedChild(ScopedChild&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedChild& operator=(ScopedChild&&) = delete;

        /// @brief Returns true when the child body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the body is visible.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::TreeNodeEx` and `CollapsingHeader`.
    ///
    /// A `TreeNodeEx` that is open owes a `TreePop`; `CollapsingHeader` has no pop
    /// counterpart. The factory bakes which rule applies into `m_Pop` so the destructor
    /// pops only for an open `TreeNode`.
    class [[nodiscard]] ScopedTree
    {
    public:
        /// @brief Constructs the guard.
        /// @param open  Whether the tree node is open.
        /// @param pop   Whether `TreePop` must be called on destruction.
        ScopedTree(bool open, bool pop) : m_Open(open), m_Pop(pop) {}

        /// @brief Calls `ImGui::TreePop` if `m_Pop` is set.
        ~ScopedTree();

        ScopedTree(const ScopedTree&) = delete;
        ScopedTree& operator=(const ScopedTree&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedTree(ScopedTree&& other) noexcept : m_Open(other.m_Open), m_Pop(other.m_Pop)
        {
            other.m_Live = false;
        }
        ScopedTree& operator=(ScopedTree&&) = delete;

        /// @brief Returns true when the tree node is open and its children should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the node is expanded.
        bool m_Open;
        /// @brief Whether `TreePop` is required on destruction.
        bool m_Pop;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginTable`/`EndTable`.
    ///
    /// `EndTable` runs only when the table opened.
    class [[nodiscard]] ScopedTable
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginTable` call.
        /// @param open  Whether the table body should be drawn.
        explicit ScopedTable(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndTable` when the table opened.
        ~ScopedTable();

        ScopedTable(const ScopedTable&) = delete;
        ScopedTable& operator=(const ScopedTable&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedTable(ScopedTable&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedTable& operator=(ScopedTable&&) = delete;

        /// @brief Returns true when the table body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the table is visible.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginMainMenuBar`/`EndMainMenuBar`.
    ///
    /// `EndMainMenuBar` runs only when the bar opened.
    class [[nodiscard]] ScopedMenuBar
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginMainMenuBar` call.
        /// @param open  Whether the menu bar is visible.
        explicit ScopedMenuBar(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndMainMenuBar` when the bar opened.
        ~ScopedMenuBar();

        ScopedMenuBar(const ScopedMenuBar&) = delete;
        ScopedMenuBar& operator=(const ScopedMenuBar&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedMenuBar(ScopedMenuBar&& other) noexcept : m_Open(other.m_Open)
        {
            other.m_Live = false;
        }
        ScopedMenuBar& operator=(ScopedMenuBar&&) = delete;

        /// @brief Returns true when the menu bar body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the menu bar is visible.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginMenu`/`EndMenu`.
    ///
    /// `EndMenu` runs only when the menu opened.
    class [[nodiscard]] ScopedMenu
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginMenu` call.
        /// @param open  Whether the menu is open.
        explicit ScopedMenu(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndMenu` when the menu opened.
        ~ScopedMenu();

        ScopedMenu(const ScopedMenu&) = delete;
        ScopedMenu& operator=(const ScopedMenu&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedMenu(ScopedMenu&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedMenu& operator=(ScopedMenu&&) = delete;

        /// @brief Returns true when the menu body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the menu is open.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginPopup`/`EndPopup`.
    ///
    /// `EndPopup` runs only when the popup is open.
    class [[nodiscard]] ScopedPopup
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginPopup` call.
        /// @param open  Whether the popup is open.
        explicit ScopedPopup(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndPopup` when the popup is open.
        ~ScopedPopup();

        ScopedPopup(const ScopedPopup&) = delete;
        ScopedPopup& operator=(const ScopedPopup&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedPopup(ScopedPopup&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedPopup& operator=(ScopedPopup&&) = delete;

        /// @brief Returns true when the popup body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the popup is open.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginDragDropSource`/`EndDragDropSource`.
    ///
    /// `EndDragDropSource` runs only when the source began (an item is being dragged).
    class [[nodiscard]] ScopedDragDropSource
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginDragDropSource` call.
        /// @param open  Whether the drag-drop source began this frame.
        explicit ScopedDragDropSource(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndDragDropSource` when the source began.
        ~ScopedDragDropSource();

        ScopedDragDropSource(const ScopedDragDropSource&) = delete;
        ScopedDragDropSource& operator=(const ScopedDragDropSource&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedDragDropSource(ScopedDragDropSource&& other) noexcept : m_Open(other.m_Open)
        {
            other.m_Live = false;
        }
        ScopedDragDropSource& operator=(ScopedDragDropSource&&) = delete;

        /// @brief Returns true when the drag-drop source began and its payload should be set.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the source began this frame.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for `ImGui::BeginDragDropTarget`/`EndDragDropTarget`.
    ///
    /// `EndDragDropTarget` runs only when the target began (the previous item can receive a drop).
    class [[nodiscard]] ScopedDragDropTarget
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginDragDropTarget` call.
        /// @param open  Whether the drag-drop target began this frame.
        explicit ScopedDragDropTarget(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndDragDropTarget` when the target began.
        ~ScopedDragDropTarget();

        ScopedDragDropTarget(const ScopedDragDropTarget&) = delete;
        ScopedDragDropTarget& operator=(const ScopedDragDropTarget&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedDragDropTarget(ScopedDragDropTarget&& other) noexcept : m_Open(other.m_Open)
        {
            other.m_Live = false;
        }
        ScopedDragDropTarget& operator=(ScopedDragDropTarget&&) = delete;

        /// @brief Returns true when the target began and a payload may be accepted.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the target began this frame.
        bool m_Open;
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Unconditional scope guard for `ImGui::BeginDisabled`/`EndDisabled`.
    ///
    /// The body always runs and the destructor always pops. `m_Live` tracks move-out
    /// so a moved-from guard's destructor is a no-op.
    class [[nodiscard]] DisabledScope
    {
    public:
        /// @brief Constructs the guard.
        DisabledScope() = default;

        /// @brief Calls `ImGui::EndDisabled` unconditionally.
        ~DisabledScope();

        DisabledScope(const DisabledScope&) = delete;
        DisabledScope& operator=(const DisabledScope&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        DisabledScope(DisabledScope&& other) noexcept { other.m_Live = false; }
        DisabledScope& operator=(DisabledScope&&) = delete;

    private:
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Unconditional scope guard for `ImGui::PushID`/`PopID`.
    class [[nodiscard]] IdScope
    {
    public:
        /// @brief Constructs the guard.
        IdScope() = default;

        /// @brief Calls `ImGui::PopID` unconditionally.
        ~IdScope();

        IdScope(const IdScope&) = delete;
        IdScope& operator=(const IdScope&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        IdScope(IdScope&& other) noexcept { other.m_Live = false; }
        IdScope& operator=(IdScope&&) = delete;

    private:
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Unconditional scope guard for `ImGui::PushStyleColor`/`PopStyleColor`.
    class [[nodiscard]] StyleColorScope
    {
    public:
        /// @brief Constructs the guard.
        StyleColorScope() = default;

        /// @brief Calls `ImGui::PopStyleColor` unconditionally.
        ~StyleColorScope();

        StyleColorScope(const StyleColorScope&) = delete;
        StyleColorScope& operator=(const StyleColorScope&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        StyleColorScope(StyleColorScope&& other) noexcept { other.m_Live = false; }
        StyleColorScope& operator=(StyleColorScope&&) = delete;

    private:
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Unconditional scope guard for `ImGui::PushStyleVar`/`PopStyleVar`.
    class [[nodiscard]] StyleVarScope
    {
    public:
        /// @brief Constructs the guard.
        StyleVarScope() = default;

        /// @brief Calls `ImGui::PopStyleVar` unconditionally.
        ~StyleVarScope();

        StyleVarScope(const StyleVarScope&) = delete;
        StyleVarScope& operator=(const StyleVarScope&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        StyleVarScope(StyleVarScope&& other) noexcept { other.m_Live = false; }
        StyleVarScope& operator=(StyleVarScope&&) = delete;

    private:
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Opens a window and returns a scope guard whose destructor calls `End` unconditionally.
    /// @param title  Window title and ImGui id.
    /// @param open   Optional pointer toggled when the close button is clicked.
    /// @param flags  Window display flags.
    [[nodiscard]] ScopedWindow Window(string_view title, bool* open = nullptr,
                                      WindowFlags flags = WindowFlags::None);

    /// @brief Opens a child region and returns a scope guard whose destructor calls `EndChild` unconditionally.
    /// @param id    ImGui id string for the child region.
    /// @param size  Requested size; zero components fill the available space.
    /// @param flags Window display flags.
    [[nodiscard]] ScopedChild Child(string_view id, vec2 size = {},
                                    WindowFlags flags = WindowFlags::None);

    /// @brief Opens a tree node and returns a scope guard that calls `TreePop` when the node is open.
    /// @param label  Display label and ImGui id.
    /// @param flags  Tree display flags.
    [[nodiscard]] ScopedTree TreeNode(string_view label, TreeFlags flags = TreeFlags::None);

    /// @brief Opens a collapsing header and returns a scope guard with no pop (header has none).
    /// @param label  Display label and ImGui id.
    /// @param flags  Tree display flags.
    [[nodiscard]] ScopedTree CollapsingHeader(string_view label, TreeFlags flags = TreeFlags::None);

    /// @brief Opens a table and returns a scope guard that calls `EndTable` when the table opened.
    ///
    /// The scope owns `BeginTable`/`EndTable`; row and column cursor calls are free functions.
    /// @param id       ImGui id string for the table.
    /// @param columns  Number of columns.
    [[nodiscard]] ScopedTable Table(string_view id, i32 columns);

    /// @brief Opens a two-column property table for aligned `label : widget` inspector rows.
    ///
    /// Column 0 (labels) is auto-sized to its content; column 1 (the widget) stretches to
    /// fill the rest. Drive it with `PropertyLabel` per row, which advances the row, draws
    /// the label in column 0, and stretches the next widget across column 1. The returned
    /// guard calls `EndTable` when the table opened.
    /// @param id  ImGui id string for the table.
    [[nodiscard]] ScopedTable PropertyTable(string_view id);

    /// @brief Declares a column with a header label.
    /// @param label  Column header text.
    void TableSetupColumn(string_view label);

    /// @brief Submits all column headers declared with `TableSetupColumn`.
    void TableHeadersRow();

    /// @brief Advances to the next row.
    void TableNextRow();

    /// @brief Advances to the next column.
    void TableNextColumn();

    /// @brief Sets the active column by index.
    /// @param column  Zero-based column index.
    void TableSetColumnIndex(i32 column);

    /// @brief Opens the main menu bar and returns a scope guard that calls `EndMainMenuBar` when opened.
    [[nodiscard]] ScopedMenuBar MainMenuBar();

    /// @brief Opens a menu and returns a scope guard that calls `EndMenu` when opened.
    /// @param label  Menu label and ImGui id.
    [[nodiscard]] ScopedMenu Menu(string_view label);

    /// @brief Returns true the frame the menu item is clicked.
    ///
    /// `MenuItem` has no begin/end pair, so it is a free function.
    /// @param label    Item label.
    /// @param enabled  When false, the item is greyed out and non-interactive.
    bool MenuItem(string_view label, bool enabled = true);

    /// @brief Toggles `*selected` when clicked and returns true the frame it changes.
    ///
    /// Used for panel-visibility checkboxes in the Window menu.
    /// @param label     Item label.
    /// @param selected  In/out toggle state.
    bool MenuItem(string_view label, bool* selected);

    /// @brief Opens a popup and returns a scope guard that calls `EndPopup` when the popup is open.
    /// @param id  ImGui id string for the popup.
    [[nodiscard]] ScopedPopup Popup(string_view id);

    /// @brief Queues a popup to open on the next frame.
    /// @param id  ImGui id string for the popup, matching a `Popup(id)` call.
    void OpenPopup(string_view id);

    /// @brief Opens a context-menu popup anchored to the previous item, on right-click.
    ///
    /// Wraps `BeginPopupContextItem`: right-clicking the previous widget opens it; the
    /// returned guard's destructor calls `EndPopup` when the popup is open.
    /// @param id  ImGui id string for the popup.
    [[nodiscard]] ScopedPopup PopupContextItem(string_view id);

    /// @brief Opens a context-menu popup for empty space in the current window, on right-click.
    ///
    /// Wraps `BeginPopupContextWindow`: right-clicking window space not over an item opens it;
    /// the returned guard's destructor calls `EndPopup` when the popup is open.
    /// @param id  ImGui id string for the popup.
    [[nodiscard]] ScopedPopup PopupContextWindow(string_view id);

    /// @brief Begins a drag-drop source on the previous item.
    ///
    /// Inside an open guard, call `SetDragDropPayload` to attach the dragged data and draw
    /// a drag preview. The guard's destructor ends the source when it began.
    [[nodiscard]] ScopedDragDropSource DragDropSource();

    /// @brief Sets the payload carried by the active drag-drop source.
    ///
    /// Copies `size` bytes; ImGui owns the copy for the drag's duration. Call only inside an
    /// open `DragDropSource` scope.
    /// @param type  Caller-defined payload type tag, matched by `AcceptDragDropPayload`.
    /// @param data  Pointer to the payload bytes to copy.
    /// @param size  Payload size in bytes.
    void SetDragDropPayload(string_view type, const void* data, usize size);

    /// @brief Begins a drag-drop target on the previous item.
    ///
    /// Inside an open guard, call `AcceptDragDropPayload` to receive a matching drop. The
    /// guard's destructor ends the target when it began.
    [[nodiscard]] ScopedDragDropTarget DragDropTarget();

    /// @brief Accepts a dropped payload of the given type on the active target.
    ///
    /// Returns a pointer to the payload bytes the frame the drop completes, else nullptr.
    /// Call only inside an open `DragDropTarget` scope.
    /// @param type  Payload type tag to match against the dragged source.
    /// @return Pointer to the payload bytes on drop, or nullptr.
    [[nodiscard]] const void* AcceptDragDropPayload(string_view type);

    /// @brief Returns a scope guard that greys out and blocks input for nested widgets.
    /// @param disabled  When false the scope is a no-op (guard still destructs safely).
    [[nodiscard]] DisabledScope Disabled(bool disabled = true);

    /// @brief Pushes an id string onto the ImGui id stack and returns a scope guard that pops it.
    /// @param id  String pushed onto the id stack.
    [[nodiscard]] IdScope PushId(string_view id);

    /// @brief Pushes a style color and returns a scope guard that pops it.
    /// @param id     Style color slot to override.
    /// @param color  RGBA color value.
    [[nodiscard]] StyleColorScope StyleColor(StyleColorId id, vec4 color);

    /// @brief Pushes a vec2 style variable and returns a scope guard that pops it.
    /// @param id     Style variable slot to override.
    /// @param value  New value.
    [[nodiscard]] StyleVarScope StyleVar(StyleVarId id, vec2 value);

    /// @brief Pushes a float style variable and returns a scope guard that pops it.
    /// @param id     Style variable slot to override.
    /// @param value  New value.
    [[nodiscard]] StyleVarScope StyleVar(StyleVarId id, f32 value);
}
