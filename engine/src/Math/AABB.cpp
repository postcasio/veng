#include <Veng/Math/AABB.h>

namespace Veng
{
    void AABB::Expand(vec3 point)
    {
        Min = glm::min(Min, point);
        Max = glm::max(Max, point);
    }

    void AABB::Expand(const AABB& other)
    {
        Min = glm::min(Min, other.Min);
        Max = glm::max(Max, other.Max);
    }

    std::array<vec3, 8> AABB::Corners() const
    {
        return {
            vec3(Min.x, Min.y, Min.z),
            vec3(Max.x, Min.y, Min.z),
            vec3(Min.x, Max.y, Min.z),
            vec3(Max.x, Max.y, Min.z),
            vec3(Min.x, Min.y, Max.z),
            vec3(Max.x, Min.y, Max.z),
            vec3(Min.x, Max.y, Max.z),
            vec3(Max.x, Max.y, Max.z),
        };
    }

    AABB AABB::Transformed(const mat4& m) const
    {
        AABB result = AABB::Empty();
        for (const vec3& corner : Corners())
        {
            const vec4 transformed = m * vec4(corner, 1.0f);
            result.Expand(vec3(transformed));
        }
        return result;
    }

    AABB Union(const AABB& a, const AABB& b)
    {
        return AABB{glm::min(a.Min, b.Min), glm::max(a.Max, b.Max)};
    }
}
