#pragma once
#include <Veng/Veng.h>

// Veng::UI item/hover queries plus a thin stats readout. Keyboard/mouse queries
// are intentionally absent — they converge with the event/input area rather than
// a parallel Veng::UI key-enum. imgui-free signatures; impl in Query.cpp.

namespace Veng::UI
{
    // True if the last item is hovered.
    [[nodiscard]] bool ItemHovered();

    // True the frame the last item was deactivated after an edit.
    [[nodiscard]] bool ItemEdited();

    // Shows a tooltip with text when the last item is hovered.
    void Tooltip(string_view text);

    // The running average frame rate (frames per second) of the UI backend.
    [[nodiscard]] f32 FrameRate();
}
