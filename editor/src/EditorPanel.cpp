#include <VengEditor/EditorPanel.h>

#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    void EditorPanel::Draw(bool* open)
    {
        const UI::WindowFlags flags = GetWindowFlags();

        // A NoScrollbar panel (e.g. a viewport) is drawn edge-to-edge; zero the
        // window padding. WindowPadding is read at Begin, so the guard need only
        // outlive the Window call.
        const bool noPadding = (flags & UI::WindowFlags::NoScrollbar) != UI::WindowFlags::None;
        optional<UI::StyleVarScope> padding;
        if (noPadding)
        {
            padding.emplace(UI::StyleVar(UI::StyleVarId::WindowPadding, vec2(0, 0)));
        }

        if (auto window = UI::Window(GetTitle(), open, flags))
        {
            OnUI();
        }
    }
}
