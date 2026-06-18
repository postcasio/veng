#pragma once
#include <Veng/Veng.h>
#include <Veng/UI/Types.h>

// Veng::UI RAII scope guards. Each guard pairs an ImGui begin/push with the
// matching end/pop in its destructor, so a body that early-returns can never
// leak the pair. A guard stores only a plain bool open-state — never an imgui
// handle — and its destructor is defined out-of-line in Scopes.cpp, so this
// header names no imgui type. A guard is [[nodiscard]] and has an
// explicit operator bool: `if (auto w = UI::Window("X")) { ... }` draws the body
// only when open.
//
// The begin/end asymmetry is baked into each guard so a call site cannot pair it
// wrong: Begin/BeginChild/BeginTable's end always runs, while
// BeginMenu/BeginMainMenuBar/BeginPopup/TreeNodeEx pop only when the begin
// returned true.

namespace Veng::UI
{
    // ImGui::Begin's End must run even when the window is collapsed, so the dtor
    // calls End() unconditionally; m_Open only gates the body via operator bool.
    class [[nodiscard]] ScopedWindow
    {
    public:
        explicit ScopedWindow(bool open) : m_Open(open) {}
        ~ScopedWindow();

        ScopedWindow(const ScopedWindow&) = delete;
        ScopedWindow& operator=(const ScopedWindow&) = delete;
        ScopedWindow(ScopedWindow&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedWindow& operator=(ScopedWindow&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // BeginChild's EndChild also runs unconditionally.
    class [[nodiscard]] ScopedChild
    {
    public:
        explicit ScopedChild(bool open) : m_Open(open) {}
        ~ScopedChild();

        ScopedChild(const ScopedChild&) = delete;
        ScopedChild& operator=(const ScopedChild&) = delete;
        ScopedChild(ScopedChild&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedChild& operator=(ScopedChild&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // TreeNodeEx / CollapsingHeader. A TreeNodeEx that is open owes a TreePop;
    // CollapsingHeader has no pop counterpart at all. The factory bakes which rule
    // applies into m_Pop so the dtor pops only for an open TreeNode.
    class [[nodiscard]] ScopedTree
    {
    public:
        ScopedTree(bool open, bool pop) : m_Open(open), m_Pop(pop) {}
        ~ScopedTree();

        ScopedTree(const ScopedTree&) = delete;
        ScopedTree& operator=(const ScopedTree&) = delete;
        ScopedTree(ScopedTree&& other) noexcept : m_Open(other.m_Open), m_Pop(other.m_Pop)
        {
            other.m_Live = false;
        }
        ScopedTree& operator=(ScopedTree&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Pop;
        bool m_Live = true;
    };

    // BeginTable's EndTable runs only when the table opened.
    class [[nodiscard]] ScopedTable
    {
    public:
        explicit ScopedTable(bool open) : m_Open(open) {}
        ~ScopedTable();

        ScopedTable(const ScopedTable&) = delete;
        ScopedTable& operator=(const ScopedTable&) = delete;
        ScopedTable(ScopedTable&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedTable& operator=(ScopedTable&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // BeginMainMenuBar's EndMainMenuBar runs only when the bar opened.
    class [[nodiscard]] ScopedMenuBar
    {
    public:
        explicit ScopedMenuBar(bool open) : m_Open(open) {}
        ~ScopedMenuBar();

        ScopedMenuBar(const ScopedMenuBar&) = delete;
        ScopedMenuBar& operator=(const ScopedMenuBar&) = delete;
        ScopedMenuBar(ScopedMenuBar&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedMenuBar& operator=(ScopedMenuBar&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // BeginMenu's EndMenu runs only when the menu opened.
    class [[nodiscard]] ScopedMenu
    {
    public:
        explicit ScopedMenu(bool open) : m_Open(open) {}
        ~ScopedMenu();

        ScopedMenu(const ScopedMenu&) = delete;
        ScopedMenu& operator=(const ScopedMenu&) = delete;
        ScopedMenu(ScopedMenu&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedMenu& operator=(ScopedMenu&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // BeginPopup's EndPopup runs only when the popup is open.
    class [[nodiscard]] ScopedPopup
    {
    public:
        explicit ScopedPopup(bool open) : m_Open(open) {}
        ~ScopedPopup();

        ScopedPopup(const ScopedPopup&) = delete;
        ScopedPopup& operator=(const ScopedPopup&) = delete;
        ScopedPopup(ScopedPopup&& other) noexcept : m_Open(other.m_Open) { other.m_Live = false; }
        ScopedPopup& operator=(ScopedPopup&&) = delete;

        explicit operator bool() const { return m_Open; }

    private:
        bool m_Open;
        bool m_Live = true;
    };

    // Unconditional scopes — the body always runs and the dtor always pops. The
    // bool member only tracks move-out so a moved-from guard's dtor is a no-op.
    class [[nodiscard]] DisabledScope
    {
    public:
        DisabledScope() = default;
        ~DisabledScope();

        DisabledScope(const DisabledScope&) = delete;
        DisabledScope& operator=(const DisabledScope&) = delete;
        DisabledScope(DisabledScope&& other) noexcept { other.m_Live = false; }
        DisabledScope& operator=(DisabledScope&&) = delete;

    private:
        bool m_Live = true;
    };

    class [[nodiscard]] IdScope
    {
    public:
        IdScope() = default;
        ~IdScope();

        IdScope(const IdScope&) = delete;
        IdScope& operator=(const IdScope&) = delete;
        IdScope(IdScope&& other) noexcept { other.m_Live = false; }
        IdScope& operator=(IdScope&&) = delete;

    private:
        bool m_Live = true;
    };

    class [[nodiscard]] StyleColorScope
    {
    public:
        StyleColorScope() = default;
        ~StyleColorScope();

        StyleColorScope(const StyleColorScope&) = delete;
        StyleColorScope& operator=(const StyleColorScope&) = delete;
        StyleColorScope(StyleColorScope&& other) noexcept { other.m_Live = false; }
        StyleColorScope& operator=(StyleColorScope&&) = delete;

    private:
        bool m_Live = true;
    };

    class [[nodiscard]] StyleVarScope
    {
    public:
        StyleVarScope() = default;
        ~StyleVarScope();

        StyleVarScope(const StyleVarScope&) = delete;
        StyleVarScope& operator=(const StyleVarScope&) = delete;
        StyleVarScope(StyleVarScope&& other) noexcept { other.m_Live = false; }
        StyleVarScope& operator=(StyleVarScope&&) = delete;

    private:
        bool m_Live = true;
    };

    // Begin/end scope factories. A window's End runs unconditionally; the rest of
    // the begin/end family follows ImGui's own conditional-end rules, encapsulated
    // in each guard's destructor.
    [[nodiscard]] ScopedWindow Window(string_view title, bool* open = nullptr,
                                      WindowFlags flags = WindowFlags::None);
    [[nodiscard]] ScopedChild Child(string_view id, vec2 size = {},
                                    WindowFlags flags = WindowFlags::None);

    [[nodiscard]] ScopedTree TreeNode(string_view label, TreeFlags flags = TreeFlags::None);
    [[nodiscard]] ScopedTree CollapsingHeader(string_view label, TreeFlags flags = TreeFlags::None);

    // Tables: the scope owns BeginTable/EndTable; the row/column cursor is not a
    // scope, so its calls are free functions.
    [[nodiscard]] ScopedTable Table(string_view id, i32 columns);
    void TableSetupColumn(string_view label);
    void TableHeadersRow();
    void TableNextRow();
    void TableNextColumn();
    void TableSetColumnIndex(i32 column);

    [[nodiscard]] ScopedMenuBar MainMenuBar();
    [[nodiscard]] ScopedMenu Menu(string_view label);

    // MenuItem has no begin/end, so it is a free function. The plain form returns
    // true the frame it is clicked; the selected form toggles *selected (the
    // Window-menu panel-visibility checkboxes).
    bool MenuItem(string_view label, bool enabled = true);
    bool MenuItem(string_view label, bool* selected);

    [[nodiscard]] ScopedPopup Popup(string_view id);
    void OpenPopup(string_view id);

    // Unconditional-scope factories.
    [[nodiscard]] DisabledScope Disabled(bool disabled = true);
    [[nodiscard]] IdScope PushId(string_view id);
    [[nodiscard]] StyleColorScope StyleColor(StyleColorId id, vec4 color);
    [[nodiscard]] StyleVarScope StyleVar(StyleVarId id, vec2 value);
    [[nodiscard]] StyleVarScope StyleVar(StyleVarId id, f32 value);
}
