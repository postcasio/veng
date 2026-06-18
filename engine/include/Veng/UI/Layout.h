#pragma once
#include <Veng/Veng.h>

// Veng::UI layout helpers — the small remainder of cursor/spacing primitives a
// widget author reaches for inline. imgui-free signatures; impl in Layout.cpp.

namespace Veng::UI
{
    void Separator();
    void SameLine();
    void Spacing();

    // Space remaining in the current content region.
    [[nodiscard]] vec2 ContentRegionAvail();

    // Scrolls so the current cursor position is visible (the console auto-scroll).
    void ScrollToHere();
}
