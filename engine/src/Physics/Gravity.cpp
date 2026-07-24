#include <Veng/Physics/Gravity.h>

#include <Veng/Assert.h>

#include <array>

namespace Veng
{
    namespace
    {
        /// @brief Signed distance from @p point to the region's boundary, positive inside.
        ///
        /// The point is brought into the region's local frame, so an oriented box or a tilted
        /// cylinder needs no per-face data. Positive is the distance to the nearest boundary from
        /// inside; negative means outside.
        /// @param region  The region to measure against.
        /// @param point   The point, in the same frame as the region.
        /// @return The signed inside-depth.
        [[nodiscard]] f32 RegionDepth(const Region& region, const vec3 point)
        {
            const vec3 local = glm::inverse(region.Orientation) * (point - region.Center);
            switch (region.Shape)
            {
            case RegionShape::Box:
            {
                const vec3 slack = region.HalfExtents - glm::abs(local);
                return glm::min(slack.x, glm::min(slack.y, slack.z));
            }
            case RegionShape::Sphere:
                return region.HalfExtents.x - glm::length(local);
            case RegionShape::Cylinder:
            {
                const f32 radial = glm::length(vec2(local.x, local.z));
                return glm::min(region.HalfExtents.x - radial,
                                region.HalfExtents.y - glm::abs(local.y));
            }
            }
            VE_ASSERT(false, "Unmapped RegionShape {}", static_cast<u32>(region.Shape));
        }

        /// @brief Normalizes @p value, returning zero when it is too short to have a direction.
        /// @param value  The vector to normalize.
        /// @return The unit vector, or zero.
        [[nodiscard]] vec3 SafeNormalize(const vec3 value)
        {
            const f32 length = glm::length(value);
            return length > 1e-6f ? value / length : vec3(0.0f);
        }

        /// @brief The Radial/Axial falloff factor at a radius, in [0, 1].
        ///
        /// Zero below @p inner, ramping linearly to one at @p outer and holding one beyond. A
        /// degenerate band (outer at or below inner) is a hard step at @p inner.
        /// @param radius  Distance from the origin or axis.
        /// @param inner   Radius below which the field is zero.
        /// @param outer   Radius at which the field reaches full strength.
        /// @return The strength factor.
        [[nodiscard]] f32 RadialFalloff(const f32 radius, const f32 inner, const f32 outer)
        {
            if (outer <= inner)
            {
                return radius >= inner ? 1.0f : 0.0f;
            }
            return glm::clamp((radius - inner) / (outer - inner), 0.0f, 1.0f);
        }

        /// @brief The field vector a single source produces at a point, before membership weighting.
        /// @param source  The world-space source.
        /// @param point   The world-space point.
        /// @return The acceleration the source alone would apply.
        [[nodiscard]] vec3 SourceGravity(const GravitySourceInstance& source, const vec3 point)
        {
            switch (source.Kind)
            {
            case GravityKind::Uniform:
                return SafeNormalize(source.Direction) * source.Magnitude;
            case GravityKind::Radial:
            {
                const vec3 toOrigin = source.Origin - point;
                const f32 radius = glm::length(toOrigin);
                return SafeNormalize(toOrigin) *
                       (source.Magnitude *
                        RadialFalloff(radius, source.InnerRadius, source.OuterRadius));
            }
            case GravityKind::Axial:
            {
                const vec3 axis = SafeNormalize(source.Direction);
                const vec3 toPoint = point - source.Origin;
                const vec3 radial = toPoint - axis * glm::dot(toPoint, axis);
                const f32 radius = glm::length(radial);
                return SafeNormalize(radial) *
                       (source.Magnitude *
                        RadialFalloff(radius, source.InnerRadius, source.OuterRadius));
            }
            }
            VE_ASSERT(false, "Unmapped GravityKind {}", static_cast<u32>(source.Kind));
        }

        /// @brief A source's membership weight at a point, given its inside-depth.
        ///
        /// Zero outside the region; one once the point is past the blend band; a linear ramp from
        /// the boundary inward across BlendWidth otherwise. A zero blend width is a hard edge.
        /// @param source  The source, for its BlendWidth.
        /// @param depth   The point's inside-depth (see RegionDepth).
        /// @return The weight in [0, 1].
        [[nodiscard]] f32 BlendWeight(const GravitySourceInstance& source, const f32 depth)
        {
            if (depth < 0.0f)
            {
                return 0.0f;
            }
            if (source.BlendWidth <= 0.0f)
            {
                return 1.0f;
            }
            return glm::clamp(depth / source.BlendWidth, 0.0f, 1.0f);
        }
    }

    bool Contains(const Region& region, const vec3 point)
    {
        return RegionDepth(region, point) >= 0.0f;
    }

    vec3 EvaluateGravity(const std::span<const GravitySourceInstance> sources, const vec3 position)
    {
        // The contained sources' contributions, kept sorted by descending priority in a stack
        // buffer. More than this many sources overlapping one point is pathological, and the
        // lowest-priority extras are exactly the ones a full-coverage blend consumes last, so
        // dropping them past the cap changes nothing a body could feel.
        struct Contribution
        {
            i32 Priority;
            f32 Weight;
            vec3 Gravity;
        };
        constexpr usize MaxOverlap = 16;
        std::array<Contribution, MaxOverlap> contributions{};
        usize count = 0;

        for (const GravitySourceInstance& source : sources)
        {
            const f32 depth = RegionDepth(source.Bounds, position);
            const f32 weight = BlendWeight(source, depth);
            if (weight <= 0.0f)
            {
                continue;
            }
            const Contribution entry{
                .Priority = source.Priority,
                .Weight = weight,
                .Gravity = SourceGravity(source, position),
            };

            // Insertion sort, highest priority first and stable within a priority (a new source of
            // equal priority lands after the ones already there).
            usize insertAt = 0;
            while (insertAt < count && contributions[insertAt].Priority >= entry.Priority)
            {
                ++insertAt;
            }
            if (insertAt >= MaxOverlap)
            {
                continue;
            }
            const usize last = count < MaxOverlap ? count : MaxOverlap - 1;
            for (usize slot = last; slot > insertAt; --slot)
            {
                contributions[slot] = contributions[slot - 1];
            }
            contributions[insertAt] = entry;
            if (count < MaxOverlap)
            {
                ++count;
            }
        }

        // Fill a unit of coverage in priority order: the top source's weight is how much of the
        // result is its field, the rest is filled by the next source down, and any coverage left
        // unfilled is free-fall — no source, no gravity.
        vec3 result(0.0f);
        f32 remaining = 1.0f;
        for (usize i = 0; i < count; ++i)
        {
            const f32 covered = glm::min(contributions[i].Weight, remaining);
            result += contributions[i].Gravity * covered;
            remaining -= covered;
            if (remaining <= 1e-6f)
            {
                break;
            }
        }
        return result;
    }
}
