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
    /// @brief Persistent hover-fade state a caller holds across frames for a hover-fade scope.
    ///
    /// A `HoverFade` scope — or the fade-aware `MainMenuBar(HoverFadeState&)` overload — reads
    /// `Alpha` to fade the widgets it wraps, then eases it toward full opacity while the pointer is
    /// over them and toward `InactiveAlpha` while it is not, so idle UI chrome recedes and returns
    /// under the cursor. The caller owns one of these per faded region and passes it to the scope
    /// each frame. Only `InactiveAlpha` and `Speed` are tunables; `Alpha` and `Primed` are
    /// scope-managed and must not be written directly.
    struct HoverFadeState
    {
        /// @brief Alpha multiplier applied to the wrapped content while it is not hovered.
        f32 InactiveAlpha = 0.3f;

        /// @brief Ease rate toward the target alpha, in alpha per second; 0 snaps with no fade.
        f32 Speed = 12.0f;

        /// @brief Eased alpha applied this frame. Scope-managed; do not set directly.
        f32 Alpha = 1.0f;

        /// @brief False until the first update seeds `Alpha`, which then snaps to the target once.
        bool Primed = false;
    };

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

    /// @brief Scope guard for a `UI::Toolbar` bar.
    ///
    /// Pairs the toolbar's `BeginChild` with `EndChild` and unwinds the style var the
    /// factory pushed, all unconditionally (mirroring `Begin`/`End`). The body
    /// always draws — a toolbar has no collapsed state — so `operator bool` is always true;
    /// the guard exists for the RAII close, not a visibility gate.
    class [[nodiscard]] ScopedToolbar
    {
    public:
        /// @brief Constructs the guard.
        ScopedToolbar() = default;

        /// @brief Ends the child region and pops the toolbar's pushed style state.
        ~ScopedToolbar();

        ScopedToolbar(const ScopedToolbar&) = delete;
        ScopedToolbar& operator=(const ScopedToolbar&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedToolbar(ScopedToolbar&& other) noexcept { other.m_Live = false; }
        ScopedToolbar& operator=(ScopedToolbar&&) = delete;

        /// @brief Always true; the toolbar body is unconditionally drawn.
        explicit operator bool() const { return true; }

    private:
        /// @brief False after a move; suppresses the destructor call.
        bool m_Live = true;
    };

    /// @brief Scope guard for a `UI::Joined` group.
    ///
    /// While the guard is live, adjacent `Veng::UI` widgets fuse into one visual control: each
    /// widget after the first is placed hard against the previous one (no spacing), and every
    /// widget in the group draws square-cornered so the row reads as a single flat block. The
    /// guard's destructor ends the group and restores the prior spacing/rounding state. The body
    /// always draws — a group has no collapsed state — so `operator bool` is always true; the
    /// guard exists for the RAII close, not a visibility gate.
    class [[nodiscard]] ScopedJoined
    {
    public:
        /// @brief Constructs the guard.
        ScopedJoined() = default;

        /// @brief Ends the joined group and restores the pushed spacing/rounding state.
        ~ScopedJoined();

        ScopedJoined(const ScopedJoined&) = delete;
        ScopedJoined& operator=(const ScopedJoined&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedJoined(ScopedJoined&& other) noexcept { other.m_Live = false; }
        ScopedJoined& operator=(ScopedJoined&&) = delete;

        /// @brief Always true; the joined group body is unconditionally drawn.
        explicit operator bool() const { return true; }

    private:
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

        /// @brief Constructs a hover-fade guard that also settles the fade and pops alpha on close.
        ///
        /// The `MainMenuBar(HoverFadeState&)` factory pushes the alpha style var before
        /// `BeginMainMenuBar`; this guard measures the bar's hover while it is current and pops that
        /// alpha on destruction.
        /// @param open  Whether the menu bar is visible.
        /// @param fade  Caller-owned fade state, read for this frame's alpha and updated on close.
        ScopedMenuBar(bool open, HoverFadeState& fade) : m_Open(open), m_Fade(&fade) {}

        /// @brief Calls `ImGui::EndMainMenuBar` when the bar opened, and settles/pops any fade.
        ~ScopedMenuBar();

        ScopedMenuBar(const ScopedMenuBar&) = delete;
        ScopedMenuBar& operator=(const ScopedMenuBar&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedMenuBar(ScopedMenuBar&& other) noexcept : m_Open(other.m_Open), m_Fade(other.m_Fade)
        {
            other.m_Live = false;
        }
        ScopedMenuBar& operator=(ScopedMenuBar&&) = delete;

        /// @brief Returns true when the menu bar body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the menu bar is visible.
        bool m_Open;
        /// @brief Optional caller-owned fade; non-null means alpha was pushed and must be popped.
        HoverFadeState* m_Fade = nullptr;
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

    /// @brief Scope guard for `ImGui::BeginCombo`/`ImGui::EndCombo`.
    ///
    /// `EndCombo` runs only when the combo's dropdown is open.
    class [[nodiscard]] ScopedCombo
    {
    public:
        /// @brief Constructs the guard with the result of a prior `BeginCombo` call.
        /// @param open  Whether the combo's dropdown is open this frame.
        explicit ScopedCombo(bool open) : m_Open(open) {}

        /// @brief Calls `ImGui::EndCombo` when the dropdown is open.
        ~ScopedCombo();

        ScopedCombo(const ScopedCombo&) = delete;
        ScopedCombo& operator=(const ScopedCombo&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedCombo(ScopedCombo&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedCombo& operator=(ScopedCombo&&) = delete;

        /// @brief Returns true when the dropdown body should be drawn.
        explicit operator bool() const { return m_Open; }

    private:
        /// @brief Whether the dropdown is open.
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

    /// @brief Scope guard that fades the widgets it wraps until the pointer hovers them.
    ///
    /// Pushes `ImGuiStyleVar_Alpha` from the caller's `HoverFadeState` and opens an ImGui group, so
    /// every widget drawn inside the scope renders at the eased alpha and shares one hover rect. On
    /// destruction it closes the group, eases the state toward full opacity when the group is
    /// hovered (else toward `InactiveAlpha`), and pops the alpha. The body always draws — the fade
    /// never hides content — so `operator bool` is always true; the guard exists for the RAII close,
    /// not a visibility gate. The wrapped widgets must sit inside a window (the group needs one);
    /// the top-of-screen main menu bar is faded through `MainMenuBar(HoverFadeState&)` instead.
    class [[nodiscard]] ScopedHoverFade
    {
    public:
        /// @brief Constructs the guard over the caller's fade state.
        /// @param state  Caller-owned fade state, read for this frame's alpha and updated on close.
        explicit ScopedHoverFade(HoverFadeState& state) : m_State(&state) {}

        /// @brief Ends the group, eases the fade toward the group's hover state, and pops the alpha.
        ~ScopedHoverFade();

        ScopedHoverFade(const ScopedHoverFade&) = delete;
        ScopedHoverFade& operator=(const ScopedHoverFade&) = delete;

        /// @brief Move constructor; invalidates the source so its destructor is a no-op.
        ScopedHoverFade(ScopedHoverFade&& other) noexcept : m_State(other.m_State)
        {
            other.m_Live = false;
        }
        ScopedHoverFade& operator=(ScopedHoverFade&&) = delete;

        /// @brief Always true; the wrapped content is unconditionally drawn.
        explicit operator bool() const { return true; }

    private:
        /// @brief Caller-owned fade state driven each frame.
        HoverFadeState* m_State;
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

    /// @brief Opens a horizontal toolbar bar and returns a scope guard that closes it.
    ///
    /// A full-width, height-auto-sizing band with interior padding, for the row of
    /// buttons/inputs at the top of a panel. Lay the controls out inside the returned scope
    /// on one line (`UI::SameLine` between them), exactly as a bare row — the bar adds the
    /// padding around them:
    /// `if (auto bar = UI::Toolbar("##toolbar")) { UI::Button(...); UI::SameLine(); ... }`.
    /// The guard's destructor ends the child, unwinds the pushed style, and leaves a small
    /// gap below, so the panel content follows without a manual separator.
    /// @param id  ImGui id string for the toolbar's child region.
    /// @return A scope guard whose body is always drawn.
    [[nodiscard]] ScopedToolbar Toolbar(string_view id);

    /// @brief Fuses adjacent widgets into one visual control and returns a scope guard.
    ///
    /// Lay a search box, a combo, and a trailing button out on one line inside the returned
    /// scope, in order, with **no** `SameLine` between them — the group places each widget hard
    /// against the previous one and draws every widget square-cornered, so the row reads as one
    /// flat fused block:
    /// `if (auto g = UI::Joined("##find")) { UI::InputText(...); UI::IconButton(...); }`.
    /// The group squares uniformly because a stock framed widget cannot round per-corner and an
    /// immediate-mode widget cannot know it is the group's last item; a set of buttons wanting
    /// the rounded-pill treatment is `ButtonGroup`, which owns its whole shape. Instrumented
    /// widgets — `InputText`/`InputTextWithHint`, `Button`, `IconButton`, `IconToggleButton`,
    /// `Combo`, and the `ButtonGroup` segments — participate; other widgets inside the scope
    /// draw normally. Groups may not nest.
    /// @param id  ImGui id string for the group.
    /// @return A scope guard whose body is always drawn.
    /// @pre No `Joined` scope is already active (nesting is asserted against).
    [[nodiscard]] ScopedJoined Joined(string_view id);

    /// @brief Pins a translucent, auto-sized overlay panel inside the current window.
    ///
    /// Opens a borderless, non-moving floating panel anchored to an edge or corner of the
    /// current window's content region, offset by `padding`, for a toolbar drawn over the
    /// scene viewport image. Its background is the theme `SurfaceRaised` at reduced alpha
    /// with the theme window rounding, so the viewport reads through behind it. The panel
    /// auto-resizes to its content; lay out widgets inside the returned scope:
    /// `if (auto bar = UI::ViewportOverlay("toolbar", OverlayAnchor::TopCenter)) { ... }`.
    ///
    /// Call inside the window the overlay should pin to, after the viewport item is laid
    /// out, so the content rect is the viewport's. The returned guard's destructor calls
    /// `End` unconditionally, pairing the panel's `Begin`.
    /// @param id       ImGui id string for the overlay panel.
    /// @param anchor   Which edge or corner of the content region the panel pins to.
    /// @param padding  Offset from the anchored edge/corner, in pixels.
    /// @return A scope guard; true inside it when the panel body should be drawn.
    [[nodiscard]] ScopedWindow ViewportOverlay(string_view id, OverlayAnchor anchor,
                                               vec2 padding = {8, 8});

    /// @brief Fades the widgets drawn inside the returned scope until the pointer hovers them.
    ///
    /// Wrap a group of widgets — a HUD toolbar, an overlay panel, a row of gizmo buttons — so they
    /// sit at a reduced alpha while idle and ease back to full opacity while the pointer is over
    /// them: `if (auto fade = UI::HoverFade(m_ChromeFade)) { ...widgets... }`. The caller holds one
    /// `HoverFadeState` per faded region across frames. The wrapped widgets must be inside a window
    /// (the scope opens an ImGui group); fade the top-of-screen main menu bar with the
    /// `MainMenuBar(HoverFadeState&)` overload instead.
    /// @param state  Caller-owned fade state, held across frames (see `HoverFadeState`).
    /// @return A scope guard whose body is always drawn.
    [[nodiscard]] ScopedHoverFade HoverFade(HoverFadeState& state);

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

    /// @brief Opens the main menu bar with a hover fade and returns a scope guard.
    ///
    /// Identical to `MainMenuBar()`, but the whole bar renders at `fade`'s eased alpha and fades
    /// back to full opacity while the pointer is over it — a bar that stays out of the way over the
    /// scene until the user reaches for it. The returned guard settles the fade and pops the pushed
    /// alpha on close.
    /// @param fade  Caller-owned fade state, held across frames (see `HoverFadeState`).
    [[nodiscard]] ScopedMenuBar MainMenuBar(HoverFadeState& fade);

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

    /// @brief Closes the popup currently being drawn.
    ///
    /// Call inside an open `Popup` scope to dismiss it after a selection (a `Selectable`
    /// inside a popup does not close it on its own, unlike a `MenuItem`).
    void CloseCurrentPopup();

    /// @brief Directs keyboard focus to the next widget submitted.
    ///
    /// Call before an `InputText` to focus it — e.g. a search box the frame a picker
    /// popup appears, so the user can type immediately.
    void SetKeyboardFocusHere();

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

    /// @brief Opens a combo box with an author-drawn dropdown and returns a scope guard.
    ///
    /// Unlike Combo(), the dropdown body is drawn by the caller, so it may hold any
    /// widgets (checkboxes, selectables). The dropdown aligns to the combo frame and
    /// carries the standard arrow; size the frame with `SetNextItemWidth` beforehand.
    /// @param id       ImGui id; prefix with `##` to suppress the trailing label.
    /// @param preview  Text shown inside the closed combo frame.
    [[nodiscard]] ScopedCombo ComboBox(string_view id, string_view preview);

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
