// Device-free resolution of a GUI draw run's clip rectangle to an attachment scissor.
//
// A run's clip is an absolute rectangle in logical points; the scissor it becomes is raw pixels on
// the attachment the runs are recorded into, and a scissor is only valid inside that attachment. A
// projected draw list's clip is the bounding box of its four projected corners, so a clip on a
// world-anchored surface seen at an angle legitimately reaches past the attachment's top or left
// edge — the region actually drawn is the intersection. Resolving it is a pure function of the
// clip, the UI scale and the target extent, so it lives here pinned by unit cases rather than only
// through a rendered image.

#pragma once

#include <Veng/Gui/DrawList.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief A pixel scissor rectangle: a non-negative offset and an extent inside the attachment.
    struct GuiScissor
    {
        /// @brief Top-left corner in attachment pixels.
        ivec2 Offset{0};
        /// @brief Width and height in attachment pixels.
        uvec2 Extent{0};
    };

    /// @brief Resolves a draw run's clip rectangle to the scissor it covers on the target.
    ///
    /// The clip is scaled by the UI magnification, snapped out to whole pixels — the corner down
    /// and the far edge up, independently, so the scissor covers every pixel the clip touches —
    /// and intersected with the target. The intersection is what keeps the offset non-negative, as
    /// the scissor VUIDs require, and it crops the far edge by whatever the near edge gained
    /// rather than sliding the region across the attachment, which would mis-clip scrolled content
    /// silently instead of loudly.
    /// @param clip          The run's clip rectangle in logical points.
    /// @param uiScale       Logical-points → attachment-pixels magnification.
    /// @param targetExtent  The attachment's pixel extent.
    /// @return The covered scissor, or nullopt when the clip lies wholly outside the target.
    [[nodiscard]] inline optional<GuiScissor>
    ResolveGuiScissor(const Gui::Rect& clip, const f32 uiScale, const uvec2 targetExtent)
    {
        const vec2 min = glm::floor(clip.Min * uiScale);
        const vec2 max = glm::ceil(clip.Max() * uiScale);
        const Gui::Rect scaled{.Min = min, .Size = max - min};
        const Gui::Rect visible =
            scaled.Intersect(Gui::Rect{.Min = vec2(0.0f), .Size = vec2(targetExtent)});
        if (visible.IsEmpty())
        {
            return std::nullopt;
        }
        return GuiScissor{.Offset = ivec2(visible.Min), .Extent = uvec2(visible.Size)};
    }
}
