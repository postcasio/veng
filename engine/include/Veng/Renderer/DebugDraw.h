#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng::Renderer
{
    /// @brief One line-segment endpoint in the debug-draw accumulator.
    ///
    /// glm-only so it lives in a public header; the world position and linear-RGBA color
    /// pack byte-for-byte into the line vertex buffer the DebugDrawScenePass rasterizes.
    struct DebugLineVertex
    {
        /// @brief World-space endpoint position.
        vec3 Position;
        /// @brief Linear RGBA color (alpha is the line's opacity before the occlusion fade).
        vec4 Color;
    };

    /// @brief One camera-facing textured billboard in the debug-draw accumulator.
    ///
    /// Instanced: the DebugDrawScenePass expands each record into a camera-facing quad whose
    /// corners are the screen-space basis vectors scaled by Size. The texture is a bindless
    /// TextureHandle the consumer supplies — the engine ships no icon content.
    struct DebugBillboard
    {
        /// @brief World-space center the quad faces the camera around.
        vec3 WorldPosition;
        /// @brief Edge length of the quad in world units at the billboard's depth.
        f32 Size;
        /// @brief Linear RGBA tint multiplied into the sampled texture.
        vec4 Color;
        /// @brief Bindless slot of the icon texture this billboard samples.
        TextureHandle Texture;
    };

    /// @brief Immediate-mode accumulator for debug lines and billboards, flushed by a SceneRenderer pass.
    ///
    /// Owned by a SceneRenderer (one per managed viewport) and reached through the SceneView
    /// channel — the same handle the app pushes its ViewState through. A caller accumulates
    /// primitives for the frame; the renderer's DebugDrawScenePass rasterizes them into the LDR
    /// scene color after tonemap (depth-aware via a g-buffer depth sample). The accumulator
    /// clears each frame, so a primitive is re-pushed every frame it should appear.
    ///
    /// The accumulator must live on the SceneRenderer, not globally: the flush pass and the depth
    /// target it tests against are per-renderer, so a single global accumulator could not
    /// composite correctly into multiple viewports (split-screen) with different cameras and depth.
    /// Veng is single-threaded; no synchronization is provided.
    class DebugDraw
    {
    public:
        /// @brief Adds a world-space line segment in one color.
        /// @param a      First endpoint, world space.
        /// @param b      Second endpoint, world space.
        /// @param color  Linear RGBA color.
        void DrawLine(vec3 a, vec3 b, vec4 color);

        /// @brief Adds the twelve edges of an axis-aligned bounding box.
        /// @param box    The box, in world space.
        /// @param color  Linear RGBA color.
        void DrawBox(const AABB& box, vec4 color);

        /// @brief Adds a three-ring great-circle wireframe sphere.
        /// @param center   Sphere center, world space.
        /// @param radius   Sphere radius in world units.
        /// @param color    Linear RGBA color.
        /// @param segments Line segments per ring (clamped to at least 4).
        void DrawSphere(vec3 center, f32 radius, vec4 color, u32 segments = 24);

        /// @brief Adds the wireframe edges of a frustum from its inverse view-projection.
        ///
        /// Unprojects the eight clip-space corners through @p invViewProjection and connects the
        /// near quad, the far quad, and the four connecting edges.
        /// @param invViewProjection  Inverse of the frustum's world → clip transform.
        /// @param color              Linear RGBA color.
        void DrawFrustum(const mat4& invViewProjection, vec4 color);

        /// @brief Adds three colored axes (X red, Y green, Z blue) from a world transform.
        /// @param transform  World transform whose basis and translation place the axes.
        /// @param scale      Axis length in world units.
        void DrawTransform(const mat4& transform, f32 scale = 1.0f);

        /// @brief Adds a camera-facing textured quad.
        /// @param worldPosition  World-space center the quad faces the camera around.
        /// @param size           Quad edge length in world units.
        /// @param texture        Bindless slot of the icon texture to sample.
        /// @param color          Linear RGBA tint; (1,1,1,1) for the unmodified texture.
        void DrawBillboard(vec3 worldPosition, f32 size, TextureHandle texture, vec4 color);

        /// @brief Discards every accumulated primitive; called by the renderer at the start of each Execute.
        void Clear();

        /// @brief Returns the accumulated line vertices (two per segment).
        [[nodiscard]] const vector<DebugLineVertex>& GetLineVertices() const
        {
            return m_LineVertices;
        }

        /// @brief Returns the accumulated billboard records.
        [[nodiscard]] const vector<DebugBillboard>& GetBillboards() const { return m_Billboards; }

        /// @brief Returns whether any primitive has been accumulated this frame.
        [[nodiscard]] bool IsEmpty() const
        {
            return m_LineVertices.empty() && m_Billboards.empty();
        }

    private:
        /// @brief Line endpoints, two consecutive entries per segment.
        vector<DebugLineVertex> m_LineVertices;
        /// @brief One record per billboard, expanded to a quad by the instanced draw.
        vector<DebugBillboard> m_Billboards;
    };
}
