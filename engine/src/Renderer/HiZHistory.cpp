#include <Veng/Renderer/HiZHistory.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace Veng::Renderer
{
    bool IsHiZHistoryValid(const HiZHistoryState& previous, const HiZHistoryState& current,
                           const f32 sceneDiagonal, const HiZHistorySettings& settings)
    {
        // The projection changing at all (FOV / near-far / aspect) misaligns the
        // footprint against last frame's depth — invalidate before any metric.
        if (previous.Projection != current.Projection)
        {
            return false;
        }

        // A translation past a fraction of the scene diagonal is a teleport-sized cut.
        // A zero-diagonal scene (no bound) makes any nonzero move invalidating.
        const f32 translationLimit = settings.TranslationFraction * sceneDiagonal;
        const f32 moved = glm::length(current.CameraPosition - previous.CameraPosition);
        if (moved > translationLimit)
        {
            return false;
        }

        // Forward-axis rotation: the angle between the normalized forward vectors.
        // The dot is clamped before acos so float error past +/-1 never yields NaN.
        const f32 cosAngle =
            std::clamp(glm::dot(previous.CameraForward, current.CameraForward), -1.0F, 1.0F);
        const f32 angle = std::acos(cosAngle);
        if (angle > settings.RotationRadians)
        {
            return false;
        }

        return true;
    }
}
