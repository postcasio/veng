#include <Veng/Gui/Placement.h>

namespace Veng::Gui
{
    vec2 ClampIntoBounds(const vec2 pos, const vec2 size, const vec2 bounds, const f32 margin)
    {
        const vec2 lower(margin);
        // Widening the range never happens here; a bounds too small for size + 2*margin
        // instead pins upper to lower, so the clamp below collapses to the margin corner.
        const vec2 upper = glm::max(bounds - margin - size, lower);
        return glm::clamp(pos, lower, upper);
    }

    vec2 AnchorBeside(const vec2 anchor, const vec2 size, const vec2 offset, const vec2 bounds,
                      const f32 margin)
    {
        return ClampIntoBounds(anchor + offset, size, bounds, margin);
    }
}
