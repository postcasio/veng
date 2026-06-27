#include <Veng/Renderer/DebugDraw.h>

#include <array>

namespace Veng::Renderer
{
    void DebugDraw::DrawLine(const vec3 a, const vec3 b, const vec4 color, const f32 width)
    {
        m_LineVertices.emplace_back(a, color, width);
        m_LineVertices.emplace_back(b, color, width);
    }

    void DebugDraw::DrawBox(const AABB& box, const vec4 color, const f32 width)
    {
        const std::array<vec3, 8> c = box.Corners();
        // Corner index bits: bit0 = X (min/max), bit1 = Y, bit2 = Z.
        // Four edges along X, four along Y, four along Z.
        static constexpr std::array<std::array<u32, 2>, 12> edges = {{
            {0, 1},
            {2, 3},
            {4, 5},
            {6, 7}, // X edges
            {0, 2},
            {1, 3},
            {4, 6},
            {5, 7}, // Y edges
            {0, 4},
            {1, 5},
            {2, 6},
            {3, 7}, // Z edges
        }};
        for (const auto& edge : edges)
        {
            DrawLine(c[edge[0]], c[edge[1]], color, width);
        }
    }

    void DebugDraw::DrawSphere(const vec3 center, const f32 radius, const vec4 color,
                               const u32 segments, const f32 width)
    {
        const u32 count = std::max(segments, 4u);
        const f32 step = glm::two_pi<f32>() / static_cast<f32>(count);

        // Three great-circle rings in the XY, XZ, and YZ planes.
        for (u32 i = 0; i < count; ++i)
        {
            const f32 a = static_cast<f32>(i) * step;
            const f32 b = static_cast<f32>(i + 1) * step;
            const vec2 pa(std::cos(a), std::sin(a));
            const vec2 pb(std::cos(b), std::sin(b));

            DrawLine(center + radius * vec3(pa.x, pa.y, 0.0f),
                     center + radius * vec3(pb.x, pb.y, 0.0f), color, width);
            DrawLine(center + radius * vec3(pa.x, 0.0f, pa.y),
                     center + radius * vec3(pb.x, 0.0f, pb.y), color, width);
            DrawLine(center + radius * vec3(0.0f, pa.x, pa.y),
                     center + radius * vec3(0.0f, pb.x, pb.y), color, width);
        }
    }

    void DebugDraw::DrawFrustum(const mat4& invViewProjection, const vec4 color, const f32 width)
    {
        // The eight clip-space corners (NDC z in [0,1] for Vulkan). near = z 0, far = z 1.
        static constexpr std::array<vec3, 8> ndc = {{
            {-1.0f, -1.0f, 0.0f},
            {1.0f, -1.0f, 0.0f},
            {1.0f, 1.0f, 0.0f},
            {-1.0f, 1.0f, 0.0f},
            {-1.0f, -1.0f, 1.0f},
            {1.0f, -1.0f, 1.0f},
            {1.0f, 1.0f, 1.0f},
            {-1.0f, 1.0f, 1.0f},
        }};
        std::array<vec3, 8> corners{};
        for (u32 i = 0; i < 8; ++i)
        {
            const vec4 world = invViewProjection * vec4(ndc[i], 1.0f);
            corners[i] = vec3(world) / world.w;
        }

        // Near quad, far quad, and the four connecting edges.
        for (u32 i = 0; i < 4; ++i)
        {
            DrawLine(corners[i], corners[(i + 1) % 4], color, width);
            DrawLine(corners[i + 4], corners[((i + 1) % 4) + 4], color, width);
            DrawLine(corners[i], corners[i + 4], color, width);
        }
    }

    void DebugDraw::DrawTransform(const mat4& transform, const f32 scale, const f32 width)
    {
        const vec3 origin = vec3(transform[3]);
        DrawLine(origin, origin + scale * vec3(transform[0]), vec4(1.0f, 0.0f, 0.0f, 1.0f), width);
        DrawLine(origin, origin + scale * vec3(transform[1]), vec4(0.0f, 1.0f, 0.0f, 1.0f), width);
        DrawLine(origin, origin + scale * vec3(transform[2]), vec4(0.0f, 0.0f, 1.0f, 1.0f), width);
    }

    void DebugDraw::DrawBillboard(const vec3 worldPosition, const f32 size,
                                  const TextureHandle texture, const vec4 color, const u32 pickId)
    {
        // Parenthesized aggregate init: the omitted PickRadius takes its default member initializer.
        m_Billboards.emplace_back(worldPosition, size, color, texture, pickId);
    }

    void DebugDraw::Clear()
    {
        m_LineVertices.clear();
        m_Billboards.clear();
    }
}
