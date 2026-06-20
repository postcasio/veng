#include <Veng/Math/Frustum.h>

namespace Veng
{
    Frustum Frustum::FromViewProjection(const mat4& viewProj)
    {
        // glm is column-major: viewProj[col][row]. clip = viewProj * p, so the
        // i-th clip component is row i of the matrix dotted with p, where row i
        // is (m[0][i], m[1][i], m[2][i], m[3][i]).
        const vec4 row1(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
        const vec4 row2(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
        const vec4 row3(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
        const vec4 row4(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

        Frustum frustum;
        frustum.Planes[0] = row4 + row1; // left   (clip.x >= -clip.w)
        frustum.Planes[1] = row4 - row1; // right  (clip.x <=  clip.w)
        frustum.Planes[2] = row4 + row2; // bottom (clip.y >= -clip.w)
        frustum.Planes[3] = row4 - row2; // top    (clip.y <=  clip.w)
        frustum.Planes[4] = row3;        // near   (clip.z >=  0, Vulkan ZO)
        frustum.Planes[5] = row4 - row3; // far    (clip.z <=  clip.w)
        return frustum;
    }

    bool Intersects(const Frustum& frustum, const AABB& box)
    {
        // Positive-vertex (p-vertex) test: for each plane, pick the box corner
        // farthest along the plane's normal (Max where the normal is positive,
        // Min where negative). If even that corner is behind the plane, the whole
        // box is outside it and the box is culled.
        for (const vec4& plane : frustum.Planes)
        {
            const vec3 normal(plane);
            const vec3 positive(normal.x >= 0.0f ? box.Max.x : box.Min.x,
                                normal.y >= 0.0f ? box.Max.y : box.Min.y,
                                normal.z >= 0.0f ? box.Max.z : box.Min.z);

            if (glm::dot(normal, positive) + plane.w < 0.0f)
            {
                return false;
            }
        }
        return true;
    }
}
