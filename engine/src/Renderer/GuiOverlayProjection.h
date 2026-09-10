#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Camera.h>

#include <glm/gtc/matrix_transform.hpp>

/// @brief Device-free projection of a world-anchored GUI overlay's document onto its virtual plane.
///
/// A world-anchored GuiOverlay carries a static model transform and a flat virtual plane of a fixed
/// authored world size; the draw list is textured onto that plane and projected through the live
/// camera. The projection is a pure function of the transform, the plane size, the document's
/// logical extent, and the camera view — no device — so it lives here as renderer-internal logic
/// pinned by unit cases (the ProjectToScreen / FrameTopology precedent) rather than only through a
/// rendered image.
namespace Veng::Renderer
{
    /// @brief Composes the world-space model transform of a world-anchored overlay plane.
    ///
    /// Translation then rotation; the plane's world extent is applied by the point mapping below,
    /// not folded into the model, so the document's logical resolution and the plane's world size
    /// stay independent knobs.
    /// @param position  The plane origin in world/scene space.
    /// @param rotation  The plane orientation.
    /// @return The world-space model matrix.
    [[nodiscard]] inline mat4 ComputeGuiOverlayModel(const vec3& position, const quat& rotation)
    {
        return glm::translate(mat4(1.0f), position) * glm::mat4_cast(rotation);
    }

    /// @brief Projects a document-space overlay point onto the world-anchored plane and to screen pixels.
    ///
    /// Maps the point's logical position to the plane's normalized surface (top-left origin, y down,
    /// like the draw list), places it on the plane of authored world size through the model, and
    /// projects it through the camera to top-left-origin screen pixels — the same clip → NDC → pixel
    /// path ProjectToScreen takes, so a point behind the eye returns nullopt (culled).
    /// @param docPoint     The point in document logical coordinates (framebuffer points, y down).
    /// @param docExtent    The document's logical extent the point is measured against.
    /// @param surfaceSize  The plane's world-space width and height.
    /// @param model        The plane's world-space model transform (see ComputeGuiOverlayModel).
    /// @param camera       The live camera the plane projects through.
    /// @param screenExtent The target pixel extent NDC maps onto.
    /// @return The projected pixel position, or nullopt when the point is behind the eye.
    [[nodiscard]] inline optional<vec2>
    ProjectGuiOverlayPoint(const vec2& docPoint, const vec2& docExtent, const vec2& surfaceSize,
                           const mat4& model, const CameraView& camera, const vec2& screenExtent)
    {
        const vec2 uv = docPoint / glm::max(docExtent, vec2(1.0f));
        // Top-left of the document (uv.y == 0) sits at the top of the plane (+y), so the y term is
        // (0.5 - uv.y); the plane's local frame is x right, y up, z its normal.
        const vec3 local((uv.x - 0.5f) * surfaceSize.x, (0.5f - uv.y) * surfaceSize.y, 0.0f);
        const vec3 world = vec3(model * vec4(local, 1.0f));
        return ProjectToScreen(camera, world, screenExtent);
    }
}
