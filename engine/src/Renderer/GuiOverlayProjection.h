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

    /// @brief The projective map from a world-anchored overlay's document points to screen pixels.
    ///
    /// The document lies on a flat plane and the camera is a pinhole, so the whole of
    /// ProjectGuiOverlayPoint collapses to one 3x3 homography: a document point `(x, y)` maps to
    /// `h = H * (x, y, 1)` and lands at pixel `h.xy / h.z`. Exact rather than fitted, so it agrees
    /// with the per-vertex projection the draw list takes everywhere on the plane — which is what lets
    /// a composite material invert it per fragment and work in the document's own frame.
    /// @param docExtent    The document's logical extent.
    /// @param surfaceSize  The plane's world-space width and height.
    /// @param model        The plane's world-space model transform (see ComputeGuiOverlayModel).
    /// @param camera       The live camera the plane projects through.
    /// @param screenExtent The target pixel extent NDC maps onto.
    /// @return The homography, document points to homogeneous screen pixels.
    [[nodiscard]] inline mat3
    ComputeGuiOverlayHomography(const vec2& docExtent, const vec2& surfaceSize, const mat4& model,
                                const CameraView& camera, const vec2& screenExtent)
    {
        // The document point to plane-local affine map of ProjectGuiOverlayPoint, as the three columns
        // a point's x, y and 1 scale; z is zero on the plane.
        const vec2 perPoint = surfaceSize / glm::max(docExtent, vec2(1.0f));
        const mat4 clipFromLocal = camera.ViewProjection() * model;
        const vec4 columns[3] = {
            clipFromLocal * vec4(perPoint.x, 0.0f, 0.0f, 0.0f),
            clipFromLocal * vec4(0.0f, -perPoint.y, 0.0f, 0.0f),
            clipFromLocal * vec4(-0.5f * surfaceSize.x, 0.5f * surfaceSize.y, 0.0f, 1.0f),
        };
        // pixel = (clip.xy / clip.w * 0.5 + 0.5) * extent = (extent / 2) * (clip.xy + clip.w) / clip.w.
        const vec2 half = screenExtent * 0.5f;
        mat3 homography;
        for (i32 column = 0; column < 3; ++column)
        {
            const vec4& clip = columns[column];
            homography[column] =
                vec3(half.x * (clip.x + clip.w), half.y * (clip.y + clip.w), clip.w);
        }
        return homography;
    }

    /// @brief The map from a screen-space overlay's document points to screen pixels: a scale.
    /// @param docExtent    The document's logical extent, stretched over the target.
    /// @param screenExtent The target pixel extent.
    /// @return The homography, document points to homogeneous screen pixels.
    [[nodiscard]] inline mat3 ComputeGuiOverlayScreenHomography(const vec2& docExtent,
                                                                const vec2& screenExtent)
    {
        const vec2 scale = screenExtent / glm::max(docExtent, vec2(1.0f));
        return mat3(vec3(scale.x, 0.0f, 0.0f), vec3(0.0f, scale.y, 0.0f), vec3(0.0f, 0.0f, 1.0f));
    }

    /// @brief Applies a homography to a point and divides through.
    /// @param homography The map (see ComputeGuiOverlayHomography or its inverse).
    /// @param point      The point to map.
    /// @return The mapped point.
    [[nodiscard]] inline vec2 ApplyGuiOverlayHomography(const mat3& homography, const vec2& point)
    {
        const vec3 mapped = homography * vec3(point, 1.0f);
        return vec2(mapped) / mapped.z;
    }

    /// @brief The pixel granule a material overlay's document rect and its intermediate round to.
    ///
    /// Coarse enough that a document drifting by a few pixels a frame keeps the same rect, so the
    /// intermediate grows in steps rather than tracking every sub-granule change.
    inline constexpr u32 GuiOverlayDocumentGranule = 64;

    /// @brief The target-pixel rectangle a material overlay's projected document covers this frame.
    struct GuiOverlayDocumentRect
    {
        /// @brief Top-left corner in target pixels; granule-aligned.
        uvec2 Origin{0, 0};
        /// @brief Width and height in target pixels; zero when the document covers nothing.
        uvec2 Size{0, 0};

        /// @brief Returns true when the rect covers no pixel.
        [[nodiscard]] bool IsEmpty() const { return Size.x == 0 || Size.y == 0; }
    };

    /// @brief Rounds @p value up to the next multiple of GuiOverlayDocumentGranule.
    [[nodiscard]] constexpr u32 RoundUpToGuiOverlayGranule(const u32 value)
    {
        return (value + GuiOverlayDocumentGranule - 1) / GuiOverlayDocumentGranule *
               GuiOverlayDocumentGranule;
    }

    /// @brief Derives a document rect from the bounds of the document's projected geometry.
    ///
    /// The bounds round outward to the granule, then clamp to the target, so every pixel a vertex
    /// of the projected geometry can cover lies inside the rect; geometry wholly outside the target
    /// yields an empty rect. The origin stays granule-aligned; only a size clamped at the target's
    /// far edge is not a granule multiple.
    /// @param boundsMin  The projected geometry's minimum corner, in target pixels.
    /// @param boundsMax  The projected geometry's maximum corner, in target pixels.
    /// @param extent     The target's pixel extent.
    /// @return The rect, or an empty rect when the bounds miss the target.
    [[nodiscard]] inline GuiOverlayDocumentRect
    ComputeGuiOverlayDocumentRect(const vec2& boundsMin, const vec2& boundsMax, const uvec2& extent)
    {
        const vec2 lo = glm::max(glm::floor(boundsMin), vec2(0.0f));
        const vec2 hi = glm::min(glm::ceil(boundsMax), vec2(extent));
        if (!(hi.x > lo.x && hi.y > lo.y))
        {
            return {};
        }
        const uvec2 origin = uvec2(lo) / GuiOverlayDocumentGranule * GuiOverlayDocumentGranule;
        const uvec2 end = glm::min(uvec2(RoundUpToGuiOverlayGranule(static_cast<u32>(hi.x)),
                                         RoundUpToGuiOverlayGranule(static_cast<u32>(hi.y))),
                                   extent);
        return {.Origin = origin, .Size = end - origin};
    }

    /// @brief The extent the shared document intermediate takes to hold a rect of @p required size.
    ///
    /// It grows to cover the requirement, rounded to the granule, and never shrinks below its
    /// high-water mark — so a document swinging into view grows it once, not every frame. The one
    /// shrink is to the target's own granule-rounded extent, when the target was resized below it.
    /// @param allocated  The intermediate's current extent; zero when unallocated.
    /// @param required   The largest document rect size this frame.
    /// @param extent     The target's pixel extent.
    /// @return The extent the intermediate should have; equal to @p allocated when nothing changes.
    [[nodiscard]] inline uvec2 GrowGuiOverlayDocumentAllocation(const uvec2& allocated,
                                                                const uvec2& required,
                                                                const uvec2& extent)
    {
        const uvec2 ceiling(RoundUpToGuiOverlayGranule(extent.x),
                            RoundUpToGuiOverlayGranule(extent.y));
        const uvec2 wanted(RoundUpToGuiOverlayGranule(glm::max(required.x, 1u)),
                           RoundUpToGuiOverlayGranule(glm::max(required.y, 1u)));
        return glm::min(glm::max(allocated, wanted), ceiling);
    }
}
