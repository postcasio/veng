#include <Veng/Renderer/InstancedMesh.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    namespace
    {
        // Below this a view-projection's w row is treated as absent: an orthographic projection.
        constexpr f32 PerspectiveEpsilon = 1e-6f;
    }

    MeshInstance MeshInstance::Make(const vec3& position, const quat& rotation, const f32 scale,
                                    const vec4& datum)
    {
        mat4 transform = glm::mat4_cast(rotation);
        transform[0] *= scale;
        transform[1] *= scale;
        transform[2] *= scale;
        transform[3] = vec4(position, 1.0f);
        return FromMatrix(transform, datum);
    }

    MeshInstance MeshInstance::FromMatrix(const mat4& transform, const vec4& datum)
    {
        // glm is column-major: transform[c][r].
        return MeshInstance{
            .Row0 = vec4(transform[0][0], transform[1][0], transform[2][0], transform[3][0]),
            .Row1 = vec4(transform[0][1], transform[1][1], transform[2][1], transform[3][1]),
            .Row2 = vec4(transform[0][2], transform[1][2], transform[2][2], transform[3][2]),
            .Datum = datum,
        };
    }

    mat4 MeshInstance::GetMatrix() const
    {
        return mat4(vec4(Row0.x, Row1.x, Row2.x, 0.0f), vec4(Row0.y, Row1.y, Row2.y, 0.0f),
                    vec4(Row0.z, Row1.z, Row2.z, 0.0f), vec4(Row0.w, Row1.w, Row2.w, 1.0f));
    }

    InstanceSet::InstanceSet()
    {
        static std::atomic<u64> s_NextId{1};
        m_Id = s_NextId.fetch_add(1, std::memory_order_relaxed);
    }

    Ref<InstanceSet> InstanceSet::Create(const u32 capacity)
    {
        Ref<InstanceSet> set = CreateRef<InstanceSet>();
        set->Resize(capacity);
        set->SetCount(capacity);
        return set;
    }

    void InstanceSet::Resize(const u32 capacity)
    {
        m_Instances.resize(capacity);
        m_Count = std::min(m_Count, capacity);
        ++m_LayoutSerial;
        ++m_WriteSerial;
        m_WholeSerial = m_WriteSerial;
        m_Writes.clear();
    }

    void InstanceSet::SetCount(const u32 count)
    {
        VE_ASSERT(count <= GetCapacity(), "InstanceSet::SetCount: {} exceeds the capacity {}",
                  count, GetCapacity());
        // The bound must hold every drawn instance, so instances coming into the drawn prefix join
        // it; a shrink leaves it conservative.
        for (u32 i = m_Count; i < count; ++i)
        {
            GrowBounds(m_Instances[i]);
        }
        m_Count = count;
    }

    void InstanceSet::Write(const u32 first, const std::span<const MeshInstance> instances)
    {
        if (instances.empty())
        {
            return;
        }
        const u32 end = first + static_cast<u32>(instances.size());
        VE_ASSERT(end <= GetCapacity(), "InstanceSet::Write: [{}, {}) exceeds the capacity {}",
                  first, end, GetCapacity());
        std::ranges::copy(instances, m_Instances.begin() + first);
        for (const MeshInstance& instance : instances)
        {
            GrowBounds(instance);
        }

        ++m_WriteSerial;
        if (m_Writes.size() == MaxTrackedWrites)
        {
            m_Writes.erase(m_Writes.begin());
        }
        m_Writes.push_back(
            WriteRecord{.Serial = m_WriteSerial, .Written = {.First = first, .End = end}});
    }

    void InstanceSet::RecomputeBounds()
    {
        m_OriginBounds = AABB::Empty();
        m_MaxScale = 0.0f;
        for (u32 i = 0; i < m_Count; ++i)
        {
            GrowBounds(m_Instances[i]);
        }
    }

    AABB InstanceSet::GetLocalBounds(const AABB& meshBounds) const
    {
        if (m_Count == 0 || m_OriginBounds.IsEmpty() || meshBounds.IsEmpty())
        {
            return AABB::Empty();
        }
        // An instance's sphere is centred on its transform of the bound's centre c — within its
        // scale times |c| of its origin — with its scale times the half-diagonal for a radius.
        const f32 reach =
            m_MaxScale * (glm::length(meshBounds.Center()) + glm::length(meshBounds.Size()) * 0.5f);
        return AABB{.Min = m_OriginBounds.Min - vec3(reach),
                    .Max = m_OriginBounds.Max + vec3(reach)};
    }

    InstanceSet::Range InstanceSet::GetWrittenSince(const u64 serial) const
    {
        if (serial >= m_WriteSerial)
        {
            return {};
        }
        const bool tracked =
            serial >= m_WholeSerial && (m_Writes.empty() || m_Writes.front().Serial <= serial + 1);
        if (!tracked)
        {
            return {.First = 0, .End = GetCapacity()};
        }
        Range written{.First = GetCapacity(), .End = 0};
        for (const WriteRecord& record : m_Writes)
        {
            if (record.Serial > serial)
            {
                written.First = std::min(written.First, record.Written.First);
                written.End = std::max(written.End, record.Written.End);
            }
        }
        return written.First < written.End ? written : Range{};
    }

    void InstanceSet::GrowBounds(const MeshInstance& instance)
    {
        m_OriginBounds.Expand(instance.GetPosition());
        m_MaxScale = std::max(m_MaxScale, MaxAxisScale(instance.GetMatrix()));
    }

    f32 MaxAxisScale(const mat4& model)
    {
        return std::max({glm::length(vec3(model[0])), glm::length(vec3(model[1])),
                         glm::length(vec3(model[2]))});
    }

    void InstanceBoundingSphere(const mat4& model, const AABB& meshBounds, vec3& center,
                                f32& radius)
    {
        center = vec3(model * vec4(meshBounds.Center(), 1.0f));
        radius = glm::length(meshBounds.Max - meshBounds.Min) * 0.5f * MaxAxisScale(model);
    }

    f32 ReferenceProjectedPixels(const mat4& referenceViewProj, const vec3& referenceEye,
                                 const f32 referenceHeightPixels, const vec3& center,
                                 const f32 radius)
    {
        // glm is column-major: viewProj[c][r]. The y row's spatial part is the projection's
        // vertical scale times a unit row of the view rotation.
        const f32 projectionScale = glm::length(
            vec3(referenceViewProj[0][1], referenceViewProj[1][1], referenceViewProj[2][1]));
        const bool perspective = glm::length(vec3(referenceViewProj[0][3], referenceViewProj[1][3],
                                                  referenceViewProj[2][3])) > PerspectiveEpsilon;
        if (!perspective)
        {
            return radius * projectionScale * referenceHeightPixels;
        }
        const f32 distance = glm::length(center - referenceEye);
        if (distance <= radius)
        {
            return std::numeric_limits<f32>::max();
        }
        return radius * projectionScale / distance * referenceHeightPixels;
    }
}
