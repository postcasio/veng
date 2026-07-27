#include <Veng/Asset/Primitives.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <Veng/Assert.h>

namespace Veng::Primitives
{
    namespace
    {
        // Records a valid material handle as the mesh's resident material and returns the index
        // submeshes reference it by; an empty handle records nothing and leaves them unassigned.
        u32 RecordMaterial(MeshData& data, AssetHandle<MaterialInstance> material)
        {
            // IsValid, never IsLoaded: a material from the asynchronous Build lands a frame or more
            // after the generator runs, and recording it by residency would drop it and bake
            // NoMaterial into the submesh, so the mesh could never draw however long the material
            // took to arrive. The draw gather already skips a submesh whose material is not yet
            // resident, so carrying the pending handle degrades for a frame instead of forever.
            if (!material.IsValid())
            {
                return SubMesh::NoMaterial;
            }

            const u32 index = static_cast<u32>(data.Materials.size());
            data.Materials.push_back(std::move(material));
            return index;
        }

        // Appends a single submesh over the full index range, carrying the recorded material.
        void FinishSubMesh(MeshData& data, AssetHandle<MaterialInstance> material)
        {
            const u32 indexCount = static_cast<u32>(data.Indices.size());
            const u32 materialIndex = RecordMaterial(data, std::move(material));

            data.SubMeshes.push_back(SubMesh{
                .IndexOffset = 0,
                .IndexCount = indexCount,
                .MaterialIndex = materialIndex,
            });
        }
    }

    MeshData Cube(f32 extent, AssetHandle<MaterialInstance> material)
    {
        const f32 h = extent * 0.5f;

        // Six faces, each with its own normal and tangent (hard edges). Per
        // face: the outward normal, the U-direction tangent, and the V
        // direction; the four corners are normal-centered plus ±U/±V. UVs span
        // the unit square; handedness w = +1 (UVs are not mirrored).
        struct Face
        {
            vec3 Normal;
            vec3 Tangent;
            vec3 Bitangent;
        };

        const Face faces[6] = {
            {.Normal = {+1.0f, 0.0f, 0.0f},
             .Tangent = {0.0f, 0.0f, -1.0f},
             .Bitangent = {0.0f, +1.0f, 0.0f}}, // +X
            {.Normal = {-1.0f, 0.0f, 0.0f},
             .Tangent = {0.0f, 0.0f, +1.0f},
             .Bitangent = {0.0f, +1.0f, 0.0f}}, // -X
            {.Normal = {0.0f, +1.0f, 0.0f},
             .Tangent = {+1.0f, 0.0f, 0.0f},
             .Bitangent = {0.0f, 0.0f, -1.0f}}, // +Y
            {.Normal = {0.0f, -1.0f, 0.0f},
             .Tangent = {+1.0f, 0.0f, 0.0f},
             .Bitangent = {0.0f, 0.0f, +1.0f}}, // -Y
            {.Normal = {0.0f, 0.0f, +1.0f},
             .Tangent = {+1.0f, 0.0f, 0.0f},
             .Bitangent = {0.0f, +1.0f, 0.0f}}, // +Z
            {.Normal = {0.0f, 0.0f, -1.0f},
             .Tangent = {-1.0f, 0.0f, 0.0f},
             .Bitangent = {0.0f, +1.0f, 0.0f}}, // -Z
        };

        MeshData data;
        data.Vertices.reserve(24);
        data.Indices.reserve(36);

        for (const Face& face : faces)
        {
            const u32 base = static_cast<u32>(data.Vertices.size());

            // Corners ordered so the index pattern below winds CCW when viewed
            // from outside (along -normal toward the face).
            const vec2 corners[4] = {
                {0.0f, 0.0f}, // -U -V
                {1.0f, 0.0f}, // +U -V
                {1.0f, 1.0f}, // +U +V
                {0.0f, 1.0f}, // -U +V
            };

            for (const vec2& uv : corners)
            {
                const f32 u = uv.x * 2.0f - 1.0f;
                const f32 v = uv.y * 2.0f - 1.0f;
                const vec3 position = (face.Normal + face.Tangent * u + face.Bitangent * v) * h;

                data.Vertices.push_back(CanonicalVertex{
                    .Position = position,
                    .Normal = face.Normal,
                    .Tangent = vec4(face.Tangent, 1.0f),
                    .UV = uv,
                });
            }

            data.Indices.push_back(base + 0);
            data.Indices.push_back(base + 1);
            data.Indices.push_back(base + 2);
            data.Indices.push_back(base + 0);
            data.Indices.push_back(base + 2);
            data.Indices.push_back(base + 3);
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Plane(vec2 size, uvec2 subdivisions, AssetHandle<MaterialInstance> material)
    {
        const u32 sx = std::max(1u, subdivisions.x);
        const u32 sz = std::max(1u, subdivisions.y);

        const vec2 half = size * 0.5f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(sx + 1) * (sz + 1));
        data.Indices.reserve(static_cast<usize>(sx) * sz * 6);

        // (sx+1) x (sz+1) grid of vertices on the XZ plane, +Y normal.
        for (u32 j = 0; j <= sz; ++j)
        {
            const f32 tz = static_cast<f32>(j) / static_cast<f32>(sz);
            for (u32 i = 0; i <= sx; ++i)
            {
                const f32 tx = static_cast<f32>(i) / static_cast<f32>(sx);

                data.Vertices.push_back(CanonicalVertex{
                    .Position =
                        vec3(half.x * (tx * 2.0f - 1.0f), 0.0f, half.y * (tz * 2.0f - 1.0f)),
                    .Normal = vec3(0.0f, 1.0f, 0.0f),
                    .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .UV = vec2(tx, tz),
                });
            }
        }

        const u32 stride = sx + 1;
        for (u32 j = 0; j < sz; ++j)
        {
            for (u32 i = 0; i < sx; ++i)
            {
                const u32 a = j * stride + i;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // CCW seen from above (+Y looking down -Y).
                data.Indices.push_back(a);
                data.Indices.push_back(c);
                data.Indices.push_back(b);

                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(d);
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData ProjectionShell(const f32 fovY, const f32 aspect, const vec2 rectCenter,
                             const vec2 rectSize, const f32 radius, const uvec2 subdivisions,
                             AssetHandle<MaterialInstance> material)
    {
        constexpr f32 Pi = 3.14159265358979323846f;
        VE_ASSERT(radius > 0.0f, "Primitives::ProjectionShell: radius must be > 0 (got {})",
                  radius);
        VE_ASSERT(aspect > 0.0f, "Primitives::ProjectionShell: aspect must be > 0 (got {})",
                  aspect);
        VE_ASSERT(fovY > 0.0f && fovY < Pi,
                  "Primitives::ProjectionShell: fovY must lie in (0, pi) radians (got {})", fovY);
        VE_ASSERT(rectSize.x > 0.0f && rectSize.y > 0.0f,
                  "Primitives::ProjectionShell: rectSize must be positive on both axes (got {}x{})",
                  rectSize.x, rectSize.y);

        const u32 nx = std::max(1u, subdivisions.x);
        const u32 ny = std::max(1u, subdivisions.y);

        const f32 tanHalfFov = std::tan(fovY * 0.5f);
        const vec2 rectMin = rectCenter - rectSize * 0.5f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(nx + 1) * (ny + 1));
        data.Indices.reserve(static_cast<usize>(nx) * ny * 6);

        // (nx+1) x (ny+1) grid over the rect. u runs left to right and v top to bottom: the rect
        // fractions and the UVs share document space's top-left origin, while camera space is +Y up,
        // hence the single negation on the vertical axis and nowhere else.
        for (u32 j = 0; j <= ny; ++j)
        {
            const f32 v = static_cast<f32>(j) / static_cast<f32>(ny);
            const f32 fractionY = rectMin.y + v * rectSize.y;
            for (u32 i = 0; i <= nx; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(nx);
                const f32 fractionX = rectMin.x + u * rectSize.x;

                // Window fraction -> NDC -> the camera-space ray at unit forward depth. The engine
                // projection bakes the Vulkan Y flip, so a top-left fraction maps to NDC directly
                // without a second flip; +Y up is what turns the y-down NDC back over.
                const vec3 ray((2.0f * fractionX - 1.0f) * aspect * tanHalfFov,
                               -(2.0f * fractionY - 1.0f) * tanHalfFov, -1.0f);
                const vec3 direction = glm::normalize(ray);

                // d(position)/du before normalization is +X, so the surface tangent is +X's component
                // in the tangent plane. Never degenerate: a ray with z = -1 can never point along X.
                const vec3 tangent =
                    glm::normalize(vec3(1.0f, 0.0f, 0.0f) - direction * direction.x);

                data.Vertices.push_back(CanonicalVertex{
                    .Position = direction * radius,
                    .Normal = -direction,
                    .Tangent = vec4(tangent, 1.0f),
                    .UV = vec2(u, v),
                });
            }
        }

        const u32 stride = nx + 1;
        for (u32 j = 0; j < ny; ++j)
        {
            for (u32 i = 0; i < nx; ++i)
            {
                const u32 a = j * stride + i;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // CCW seen from the local origin: +u steps along camera +X while +v steps along
                // camera -Y, so cross(c - a, b - a) points back at the eye, matching the normals.
                data.Indices.push_back(a);
                data.Indices.push_back(c);
                data.Indices.push_back(b);

                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(d);
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    f32 ProjectionShellReprojectionBound(const f32 fovY, const f32 aspect, const vec2 rectCenter,
                                         const vec2 rectSize, const uvec2 subdivisions,
                                         const vec2 windowExtent)
    {
        constexpr f32 Pi = 3.14159265358979323846f;
        VE_ASSERT(aspect > 0.0f,
                  "Primitives::ProjectionShellReprojectionBound: aspect must be > 0 (got {})",
                  aspect);
        VE_ASSERT(fovY > 0.0f && fovY < Pi,
                  "Primitives::ProjectionShellReprojectionBound: fovY must lie in (0, pi) radians "
                  "(got {})",
                  fovY);
        VE_ASSERT(rectSize.x > 0.0f && rectSize.y > 0.0f,
                  "Primitives::ProjectionShellReprojectionBound: rectSize must be positive on both "
                  "axes (got {}x{})",
                  rectSize.x, rectSize.y);
        VE_ASSERT(windowExtent.x > 0.0f && windowExtent.y > 0.0f,
                  "Primitives::ProjectionShellReprojectionBound: windowExtent must be positive on "
                  "both axes (got {}x{})",
                  windowExtent.x, windowExtent.y);

        const f32 tanHalfFov = std::tan(fovY * 0.5f);
        // The two squared frustum half-extents at unit forward depth: r(a)^2 = 1 + kx*ax^2 + ky*ay^2.
        const f32 kx = aspect * aspect * tanHalfFov * tanHalfFov;
        const f32 ky = tanHalfFov * tanHalfFov;

        const u32 nx = std::max(1u, subdivisions.x);
        const u32 ny = std::max(1u, subdivisions.y);

        // One cell, in window fractions and in logical points; the worst chord across it is its
        // diagonal.
        const vec2 cellFraction(rectSize.x / static_cast<f32>(nx),
                                rectSize.y / static_cast<f32>(ny));
        const vec2 cellPoints = cellFraction * windowExtent;
        const f32 chord = glm::length(cellPoints);

        // The rect's reach in frustum coordinates a = 2f - 1, per axis.
        const vec2 reach(std::abs(2.0f * rectCenter.x - 1.0f) + rectSize.x,
                         std::abs(2.0f * rectCenter.y - 1.0f) + rectSize.y);

        const f32 px = kx * cellFraction.x;
        const f32 py = ky * cellFraction.y;
        const auto gradientOverRadius = [&](const f32 ax, const f32 ay)
        { return (px * ax + py * ay) / (1.0f + kx * ax * ax + ky * ay * ay); };

        // The maximum of that rational function over the box [0, reach.x] x [0, reach.y] lies at its
        // interior stationary point, on an edge's own stationary point, or at a corner. Each candidate
        // has a closed form; collecting the coordinates and evaluating the cross product covers every
        // case with no branch on which one won.
        const f32 aspectOfCell = cellFraction.y / cellFraction.x;
        const f32 curvature = kx + ky * aspectOfCell * aspectOfCell;
        const f32 interiorX = 1.0f / std::sqrt(curvature);

        // On the edge ax = reach.x the free ay solves ay^2 + 2*(px/py)*reach.x*ay - r_x^2/ky = 0.
        const f32 radiusAtEdgeX = 1.0f + kx * reach.x * reach.x;
        const f32 edgeY = (-px * reach.x +
                           std::sqrt(px * px * reach.x * reach.x + py * py * radiusAtEdgeX / ky)) /
                          py;
        const f32 radiusAtEdgeY = 1.0f + ky * reach.y * reach.y;
        const f32 edgeX = (-py * reach.y +
                           std::sqrt(py * py * reach.y * reach.y + px * px * radiusAtEdgeY / kx)) /
                          px;

        const f32 candidatesX[5] = {0.0f, reach.x, interiorX, edgeX, 1.0f / std::sqrt(kx)};
        const f32 candidatesY[5] = {0.0f, reach.y, interiorX * aspectOfCell, edgeY,
                                    1.0f / std::sqrt(ky)};

        f32 worst = 0.0f;
        for (const f32 candidateX : candidatesX)
        {
            for (const f32 candidateY : candidatesY)
            {
                worst = std::max(worst, gradientOverRadius(glm::clamp(candidateX, 0.0f, reach.x),
                                                           glm::clamp(candidateY, 0.0f, reach.y)));
            }
        }

        return worst * chord * 0.5f;
    }

    MeshData CurvedPanel(const vec2 size, const f32 curvatureRadius, const uvec2 subdivisions,
                         AssetHandle<MaterialInstance> material)
    {
        VE_ASSERT(curvatureRadius > 0.0f,
                  "Primitives::CurvedPanel: curvatureRadius must be > 0 (got {})", curvatureRadius);
        VE_ASSERT(size.x > 0.0f && size.y > 0.0f,
                  "Primitives::CurvedPanel: size must be positive on both axes (got {}x{})", size.x,
                  size.y);

        const u32 nx = std::max(1u, subdivisions.x);
        const u32 ny = std::max(1u, subdivisions.y);

        // The panel spans [-halfAngle, +halfAngle] about the axis. A column's angle is linear in u
        // because size.x is an arc length and the columns are spaced uniformly along it.
        const f32 halfAngle = size.x * 0.5f / curvatureRadius;
        const f32 halfHeight = size.y * 0.5f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(nx + 1) * (ny + 1));
        data.Indices.reserve(static_cast<usize>(nx) * ny * 6);

        for (u32 j = 0; j <= ny; ++j)
        {
            const f32 v = static_cast<f32>(j) / static_cast<f32>(ny);
            // UVs are top-left origin and y-down while the panel is +Y up, hence the single negation
            // on the vertical axis and nowhere else.
            const f32 y = halfHeight - v * size.y;
            for (u32 i = 0; i <= nx; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(nx);
                const f32 angle = (u * 2.0f - 1.0f) * halfAngle;
                const f32 sinAngle = std::sin(angle);
                const f32 cosAngle = std::cos(angle);

                // The axis runs through (0, y, +curvatureRadius), so the surface point is one radius
                // from it and the panel's centre column sits at the local origin. The sagitta is
                // written as 2R·sin²(angle/2) rather than R·(1 - cos angle), which keeps its
                // precision as the panel flattens and the cosine approaches 1.
                const f32 halfSin = std::sin(angle * 0.5f);
                const f32 sagitta = 2.0f * curvatureRadius * halfSin * halfSin;

                // The normal points back at the axis, so the panel faces +Z and its flanks bend
                // toward a viewer there; the tangent is d(position)/d(angle), already unit length.
                data.Vertices.push_back(CanonicalVertex{
                    .Position = vec3(curvatureRadius * sinAngle, y, sagitta),
                    .Normal = vec3(-sinAngle, 0.0f, cosAngle),
                    .Tangent = vec4(cosAngle, 0.0f, sinAngle, 1.0f),
                    .UV = vec2(u, v),
                });
            }
        }

        const u32 stride = nx + 1;
        for (u32 j = 0; j < ny; ++j)
        {
            for (u32 i = 0; i < nx; ++i)
            {
                const u32 a = j * stride + i;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // CCW seen from +Z: +u steps along +X while +v steps along -Y, so
                // cross(c - a, b - a) points at +Z, matching the normals.
                data.Indices.push_back(a);
                data.Indices.push_back(c);
                data.Indices.push_back(b);

                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(d);
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    optional<vec2> CurvedPanelHit(const vec2 size, const f32 curvatureRadius, const vec3 origin,
                                  const vec3 direction)
    {
        VE_ASSERT(curvatureRadius > 0.0f,
                  "Primitives::CurvedPanelHit: curvatureRadius must be > 0 (got {})",
                  curvatureRadius);
        VE_ASSERT(size.x > 0.0f && size.y > 0.0f,
                  "Primitives::CurvedPanelHit: size must be positive on both axes (got {}x{})",
                  size.x, size.y);

        // Ray against the infinite cylinder of radius curvatureRadius about the line x = 0,
        // z = curvatureRadius running along Y: the height component drops out of the quadratic
        // entirely, so the solve is two-dimensional in XZ.
        const f32 a = direction.x * direction.x + direction.z * direction.z;
        if (a <= 0.0f)
        {
            // A ray along the axis lies either everywhere or nowhere on the surface; neither is a hit.
            return std::nullopt;
        }

        const f32 b = 2.0f * (origin.x * direction.x + (origin.z - curvatureRadius) * direction.z);
        // Expanded rather than written as |origin - axis|^2 - radius^2: that form differences two
        // squares of magnitude radius^2, and a near-flat panel's large radius reduces the remainder
        // to rounding noise.
        const f32 c = origin.x * origin.x + origin.z * origin.z - 2.0f * curvatureRadius * origin.z;
        const f32 discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f)
        {
            return std::nullopt;
        }

        // The stable quadratic. One of (-b ± sqrt(discriminant)) cancels, and at a large radius both
        // terms are of order radius, so the textbook form loses exactly the root that matters.
        const f32 q = -0.5f * (b + std::copysign(std::sqrt(discriminant), b));
        if (q == 0.0f)
        {
            // A double root at t = 0: the ray starts on the surface and never re-enters it.
            return std::nullopt;
        }

        const f32 first = q / a;
        const f32 second = c / q;
        const f32 distances[2] = {std::min(first, second), std::max(first, second)};

        const f32 halfAngle = size.x * 0.5f / curvatureRadius;
        const f32 halfHeight = size.y * 0.5f;

        // Ascending, so the first root that lands on the panel's front is the nearest such hit — not
        // merely the nearest root, which for an eye outside the cylinder is the back-facing entry
        // point and often outside the panel's extent as well.
        for (const f32 hitDistance : distances)
        {
            if (hitDistance <= 0.0f)
            {
                continue;
            }

            const vec3 point = origin + direction * hitDistance;
            if (std::abs(point.y) > halfHeight)
            {
                continue;
            }

            // The angle about the axis, measured the way the generator measures it: sine along +X,
            // cosine from the axis out toward the panel's centre column.
            const f32 angle = std::atan2(point.x, curvatureRadius - point.z);
            if (std::abs(angle) > halfAngle)
            {
                continue;
            }

            // Front face only. The outward normal points back at the axis (unnormalized here, which
            // a sign test does not care about), so a ray arriving from the +Z side has a negative dot.
            const vec3 normal(-point.x, 0.0f, curvatureRadius - point.z);
            if (glm::dot(direction, normal) >= 0.0f)
            {
                continue;
            }

            return vec2(0.5f + angle / (2.0f * halfAngle), 0.5f - point.y / size.y);
        }

        return std::nullopt;
    }

    vec2 CurvedPanelSizeForRect(const f32 fovY, const f32 aspect, const vec2 rectSize,
                                const f32 distance, const f32 curvatureRadius)
    {
        constexpr f32 Pi = 3.14159265358979323846f;
        constexpr f64 QuarterTurn = 1.57079632679489661923;
        VE_ASSERT(aspect > 0.0f, "Primitives::CurvedPanelSizeForRect: aspect must be > 0 (got {})",
                  aspect);
        VE_ASSERT(fovY > 0.0f && fovY < Pi,
                  "Primitives::CurvedPanelSizeForRect: fovY must lie in (0, pi) radians (got {})",
                  fovY);
        VE_ASSERT(distance > 0.0f,
                  "Primitives::CurvedPanelSizeForRect: distance must be > 0 (got {})", distance);
        VE_ASSERT(curvatureRadius > 0.0f,
                  "Primitives::CurvedPanelSizeForRect: curvatureRadius must be > 0 (got {})",
                  curvatureRadius);
        VE_ASSERT(rectSize.x > 0.0f && rectSize.y > 0.0f,
                  "Primitives::CurvedPanelSizeForRect: rectSize must be positive on both axes (got "
                  "{}x{})",
                  rectSize.x, rectSize.y);

        // Evaluated in double: as the panel flattens the two terms of the half-arc cancel to
        // O(distance/curvatureRadius) and their difference is then multiplied by the radius, so f32
        // cancellation would surface as noise in the near-flat width.
        const f64 tanHalfFov = std::tan(static_cast<f64>(fovY) * 0.5);
        const f64 eyeDistance = static_cast<f64>(distance);
        const f64 radius = static_cast<f64>(curvatureRadius);

        // The rect's horizontal edge ray as a screen tangent. A rect fraction spans that fraction of
        // the whole window, so its half-extent in NDC is the fraction itself.
        const f64 edgeTangent =
            static_cast<f64>(rectSize.x) * static_cast<f64>(aspect) * tanHalfFov;
        const f64 alpha = std::atan(edgeTangent);
        const f64 argument = (eyeDistance - radius) * std::sin(alpha) / radius;

        // phi_edge, the half-arc the panel subtends about its own axis. An argument past 1 has no
        // solution — the eye is outside the cylinder and the edge ray misses it — so the arc clamps
        // to the silhouette, the widest this curvature admits from that eye.
        const f64 halfArc = argument > 1.0
                                ? std::asin(radius / (eyeDistance - radius)) + QuarterTurn
                                : alpha + std::asin(argument);

        return vec2(
            static_cast<f32>(2.0 * radius * halfArc),
            static_cast<f32>(2.0 * eyeDistance * tanHalfFov * static_cast<f64>(rectSize.y)));
    }

    MeshData Sphere(f32 radius, u32 rings, u32 segments, AssetHandle<MaterialInstance> material)
    {
        rings = std::max(3u, rings);
        segments = std::max(3u, segments);

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(rings + 1) * (segments + 1));
        data.Indices.reserve(static_cast<usize>(rings) * segments * 6);

        constexpr f32 Pi = 3.14159265358979323846f;

        // (rings+1) latitude rows x (segments+1) longitude columns. theta runs
        // 0..pi from the +Y pole; phi runs 0..2pi. The seam column (phi = 2pi)
        // duplicates the phi = 0 verts with UV.x = 1 so UVs do not wrap.
        for (u32 r = 0; r <= rings; ++r)
        {
            const f32 vt = static_cast<f32>(r) / static_cast<f32>(rings);
            const f32 theta = vt * Pi;
            const f32 sinTheta = std::sin(theta);
            const f32 cosTheta = std::cos(theta);

            for (u32 s = 0; s <= segments; ++s)
            {
                const f32 vu = static_cast<f32>(s) / static_cast<f32>(segments);
                const f32 phi = vu * 2.0f * Pi;
                const f32 sinPhi = std::sin(phi);
                const f32 cosPhi = std::cos(phi);

                const vec3 normal = vec3(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);

                // d(position)/d(phi), normalized: the +U (longitude) direction.
                // Degenerate at the poles (sinTheta = 0); fall back to +X there.
                vec3 tangent = vec3(-sinPhi, 0.0f, cosPhi);
                if (sinTheta <= 1e-6f)
                {
                    tangent = vec3(1.0f, 0.0f, 0.0f);
                }

                data.Vertices.push_back(CanonicalVertex{
                    .Position = normal * radius,
                    .Normal = normal,
                    .Tangent = vec4(glm::normalize(tangent), 1.0f),
                    .UV = vec2(vu, vt),
                });
            }
        }

        const u32 stride = segments + 1;
        for (u32 r = 0; r < rings; ++r)
        {
            for (u32 s = 0; s < segments; ++s)
            {
                const u32 a = r * stride + s;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // Skip the degenerate triangle at each pole cap (one edge
                // collapses to the pole), emitting only the non-degenerate one.
                if (r != 0)
                {
                    data.Indices.push_back(a);
                    data.Indices.push_back(b);
                    data.Indices.push_back(c);
                }
                if (r != rings - 1)
                {
                    data.Indices.push_back(b);
                    data.Indices.push_back(d);
                    data.Indices.push_back(c);
                }
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Icosphere(f32 radius, u32 subdivisions, AssetHandle<MaterialInstance> material)
    {
        constexpr f32 Pi = 3.14159265358979323846f;

        // Base icosahedron: 12 unit-length directions (golden-ratio rectangle
        // corners). Positions/normals/UVs are derived from these at the end.
        const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        vector<vec3> dirs = {
            glm::normalize(vec3(-1.0f, t, 0.0f)),  glm::normalize(vec3(1.0f, t, 0.0f)),
            glm::normalize(vec3(-1.0f, -t, 0.0f)), glm::normalize(vec3(1.0f, -t, 0.0f)),
            glm::normalize(vec3(0.0f, -1.0f, t)),  glm::normalize(vec3(0.0f, 1.0f, t)),
            glm::normalize(vec3(0.0f, -1.0f, -t)), glm::normalize(vec3(0.0f, 1.0f, -t)),
            glm::normalize(vec3(t, 0.0f, -1.0f)),  glm::normalize(vec3(t, 0.0f, 1.0f)),
            glm::normalize(vec3(-t, 0.0f, -1.0f)), glm::normalize(vec3(-t, 0.0f, 1.0f)),
        };

        // The 20 outward-facing (CCW seen from outside) icosahedron triangles.
        vector<uvec3> faces = {
            {0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
            {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
            {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1},
        };

        // Each pass splits every triangle into four, inserting a unit-length
        // vertex at each edge midpoint. Midpoints are deduplicated per edge so a
        // shared edge yields one vertex.
        for (u32 pass = 0; pass < subdivisions; ++pass)
        {
            unordered_map<u64, u32> midpoints;
            auto midpoint = [&](u32 a, u32 b) -> u32
            {
                const u64 key = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
                if (const auto it = midpoints.find(key); it != midpoints.end())
                {
                    return it->second;
                }
                const u32 index = static_cast<u32>(dirs.size());
                dirs.push_back(glm::normalize(dirs[a] + dirs[b]));
                midpoints.emplace(key, index);
                return index;
            };

            vector<uvec3> next;
            next.reserve(faces.size() * 4);
            for (const uvec3& f : faces)
            {
                const u32 ab = midpoint(f.x, f.y);
                const u32 bc = midpoint(f.y, f.z);
                const u32 ca = midpoint(f.z, f.x);
                next.emplace_back(f.x, ab, ca);
                next.emplace_back(f.y, bc, ab);
                next.emplace_back(f.z, ca, bc);
                next.emplace_back(ab, bc, ca);
            }
            faces = std::move(next);
        }

        // Equirectangular UVs: u = longitude/2pi (0 at +X, wrapping at -X),
        // v = 0 at the +Y pole to 1 at the -Y pole. No base or midpoint vertex
        // lands exactly on a pole, so atan2 is always well-defined.
        auto uvOf = [&](const vec3& d)
        {
            f32 u = std::atan2(d.z, d.x) / (2.0f * Pi);
            if (u < 0.0f)
            {
                u += 1.0f;
            }
            return vec2(u, std::acos(std::clamp(d.y, -1.0f, 1.0f)) / Pi);
        };

        MeshData data;
        data.Vertices.reserve(dirs.size());
        for (const vec3& d : dirs)
        {
            // East-pointing tangent (increasing longitude); undefined at the
            // poles, where it falls back to +X.
            vec3 tangent = glm::cross(d, vec3(0.0f, 1.0f, 0.0f));
            tangent =
                glm::length(tangent) > 1e-6f ? glm::normalize(tangent) : vec3(1.0f, 0.0f, 0.0f);
            data.Vertices.push_back(CanonicalVertex{
                .Position = d * radius,
                .Normal = d,
                .Tangent = vec4(tangent, 1.0f),
                .UV = uvOf(d),
            });
        }

        // A triangle straddling the u = 0/1 wrap interpolates its UV the long way
        // around the sphere, smearing the whole texture across it. Detect the
        // straddle (u-span > 0.5) and re-emit its low-u corners against duplicate
        // vertices carrying u + 1, so the triangle's UVs stay contiguous.
        unordered_map<u32, u32> wrapped;
        auto wrap = [&](u32 index) -> u32
        {
            if (const auto it = wrapped.find(index); it != wrapped.end())
            {
                return it->second;
            }
            CanonicalVertex v = data.Vertices[index];
            v.UV.x += 1.0f;
            const u32 dup = static_cast<u32>(data.Vertices.size());
            data.Vertices.push_back(v);
            wrapped.emplace(index, dup);
            return dup;
        };

        data.Indices.reserve(faces.size() * 3);
        for (const uvec3& f : faces)
        {
            u32 i0 = f.x;
            u32 i1 = f.y;
            u32 i2 = f.z;
            const f32 u0 = data.Vertices[i0].UV.x;
            const f32 u1 = data.Vertices[i1].UV.x;
            const f32 u2 = data.Vertices[i2].UV.x;
            if (std::max({u0, u1, u2}) - std::min({u0, u1, u2}) > 0.5f)
            {
                if (u0 < 0.5f)
                {
                    i0 = wrap(i0);
                }
                if (u1 < 0.5f)
                {
                    i1 = wrap(i1);
                }
                if (u2 < 0.5f)
                {
                    i2 = wrap(i2);
                }
            }
            data.Indices.push_back(i0);
            data.Indices.push_back(i1);
            data.Indices.push_back(i2);
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Cylinder(f32 radius, f32 height, u32 segments, AssetHandle<MaterialInstance> material)
    {
        segments = std::max(3u, segments);

        constexpr f32 Pi = 3.14159265358979323846f;
        const f32 halfH = height * 0.5f;

        MeshData data;
        // Side: (segments+1) columns x 2 rows. Each cap: 1 center + (segments+1) rim.
        data.Vertices.reserve(static_cast<usize>(segments + 1) * 2 + (segments + 2) * 2);
        data.Indices.reserve(static_cast<usize>(segments) * 6 + static_cast<usize>(segments) * 6);

        // Side band: outward radial normals, +U around, +V up. The seam column
        // (s = segments) duplicates s = 0 with UV.x = 1 so UVs do not wrap.
        const u32 sideBase = static_cast<u32>(data.Vertices.size());
        for (u32 s = 0; s <= segments; ++s)
        {
            const f32 u = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 phi = u * 2.0f * Pi;
            const f32 cosPhi = std::cos(phi);
            const f32 sinPhi = std::sin(phi);

            const vec3 normal = vec3(cosPhi, 0.0f, sinPhi);
            const vec4 tangent = vec4(-sinPhi, 0.0f, cosPhi, 1.0f);

            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(cosPhi * radius, -halfH, sinPhi * radius),
                .Normal = normal,
                .Tangent = tangent,
                .UV = vec2(u, 0.0f),
            });
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(cosPhi * radius, +halfH, sinPhi * radius),
                .Normal = normal,
                .Tangent = tangent,
                .UV = vec2(u, 1.0f),
            });
        }

        for (u32 s = 0; s < segments; ++s)
        {
            const u32 a = sideBase + s * 2;       // bottom of column s
            const u32 b = a + 1;                  // top of column s
            const u32 c = sideBase + (s + 1) * 2; // bottom of column s+1
            const u32 d = c + 1;                  // top of column s+1

            // CCW seen from outside.
            data.Indices.push_back(a);
            data.Indices.push_back(b);
            data.Indices.push_back(c);
            data.Indices.push_back(b);
            data.Indices.push_back(d);
            data.Indices.push_back(c);
        }

        // Each cap is a center vertex plus a rim fan, with a hard ±Y normal.
        auto appendCap = [&](f32 y, const vec3& normal, bool flip)
        {
            const u32 center = static_cast<u32>(data.Vertices.size());
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(0.0f, y, 0.0f),
                .Normal = normal,
                .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                .UV = vec2(0.5f, 0.5f),
            });

            const u32 rimBase = static_cast<u32>(data.Vertices.size());
            for (u32 s = 0; s <= segments; ++s)
            {
                const f32 phi = static_cast<f32>(s) / static_cast<f32>(segments) * 2.0f * Pi;
                const f32 cosPhi = std::cos(phi);
                const f32 sinPhi = std::sin(phi);
                data.Vertices.push_back(CanonicalVertex{
                    .Position = vec3(cosPhi * radius, y, sinPhi * radius),
                    .Normal = normal,
                    .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                    .UV = vec2(cosPhi * 0.5f + 0.5f, sinPhi * 0.5f + 0.5f),
                });
            }

            for (u32 s = 0; s < segments; ++s)
            {
                const u32 r0 = rimBase + s;
                const u32 r1 = rimBase + s + 1;
                if (flip)
                {
                    data.Indices.push_back(center);
                    data.Indices.push_back(r0);
                    data.Indices.push_back(r1);
                }
                else
                {
                    data.Indices.push_back(center);
                    data.Indices.push_back(r1);
                    data.Indices.push_back(r0);
                }
            }
        };

        // Top cap (+Y, CCW from above), bottom cap (-Y, wound the other way).
        appendCap(+halfH, vec3(0.0f, 1.0f, 0.0f), false);
        appendCap(-halfH, vec3(0.0f, -1.0f, 0.0f), true);

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Cone(f32 radius, f32 height, u32 segments, AssetHandle<MaterialInstance> material)
    {
        segments = std::max(3u, segments);

        constexpr f32 Pi = 3.14159265358979323846f;
        const f32 halfH = height * 0.5f;

        MeshData data;
        // Side: per segment an apex + two base verts. Cap: 1 center + (segments+1) rim.
        data.Vertices.reserve(static_cast<usize>(segments) * 3 + (segments + 2));
        data.Indices.reserve(static_cast<usize>(segments) * 3 + static_cast<usize>(segments) * 3);

        // Slant of the side surface: the radial outward direction tilts up by the
        // ratio of radius to height so the normal is perpendicular to the face.
        const f32 slant = std::sqrt(radius * radius + height * height);
        const f32 ny = slant > 1e-6f ? radius / slant : 1.0f;
        const f32 nr = slant > 1e-6f ? height / slant : 0.0f;

        // Side faces: each triangle gets its own apex vertex carrying the face's
        // slanted normal (a shared smooth apex normal is ill-defined for a cone).
        for (u32 s = 0; s < segments; ++s)
        {
            const f32 phi0 = static_cast<f32>(s) / static_cast<f32>(segments) * 2.0f * Pi;
            const f32 phi1 = static_cast<f32>(s + 1) / static_cast<f32>(segments) * 2.0f * Pi;
            const f32 phiMid = (phi0 + phi1) * 0.5f;

            const vec3 baseDir0 = vec3(std::cos(phi0), 0.0f, std::sin(phi0));
            const vec3 baseDir1 = vec3(std::cos(phi1), 0.0f, std::sin(phi1));

            const vec3 normal0 = glm::normalize(vec3(baseDir0.x * nr, ny, baseDir0.z * nr));
            const vec3 normal1 = glm::normalize(vec3(baseDir1.x * nr, ny, baseDir1.z * nr));
            const vec3 apexNormal =
                glm::normalize(vec3(std::cos(phiMid) * nr, ny, std::sin(phiMid) * nr));

            const u32 base = static_cast<u32>(data.Vertices.size());
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(0.0f, +halfH, 0.0f),
                .Normal = apexNormal,
                .Tangent = vec4(-std::sin(phiMid), 0.0f, std::cos(phiMid), 1.0f),
                .UV = vec2((static_cast<f32>(s) + 0.5f) / static_cast<f32>(segments), 1.0f),
            });
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(baseDir0.x * radius, -halfH, baseDir0.z * radius),
                .Normal = normal0,
                .Tangent = vec4(-std::sin(phi0), 0.0f, std::cos(phi0), 1.0f),
                .UV = vec2(static_cast<f32>(s) / static_cast<f32>(segments), 0.0f),
            });
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(baseDir1.x * radius, -halfH, baseDir1.z * radius),
                .Normal = normal1,
                .Tangent = vec4(-std::sin(phi1), 0.0f, std::cos(phi1), 1.0f),
                .UV = vec2(static_cast<f32>(s + 1) / static_cast<f32>(segments), 0.0f),
            });

            // CCW seen from outside.
            data.Indices.push_back(base);
            data.Indices.push_back(base + 2);
            data.Indices.push_back(base + 1);
        }

        // Bottom cap fan, hard -Y normal, wound CCW seen from below.
        const vec3 capNormal = vec3(0.0f, -1.0f, 0.0f);
        const u32 center = static_cast<u32>(data.Vertices.size());
        data.Vertices.push_back(CanonicalVertex{
            .Position = vec3(0.0f, -halfH, 0.0f),
            .Normal = capNormal,
            .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
            .UV = vec2(0.5f, 0.5f),
        });
        const u32 rimBase = static_cast<u32>(data.Vertices.size());
        for (u32 s = 0; s <= segments; ++s)
        {
            const f32 phi = static_cast<f32>(s) / static_cast<f32>(segments) * 2.0f * Pi;
            const f32 cosPhi = std::cos(phi);
            const f32 sinPhi = std::sin(phi);
            data.Vertices.push_back(CanonicalVertex{
                .Position = vec3(cosPhi * radius, -halfH, sinPhi * radius),
                .Normal = capNormal,
                .Tangent = vec4(1.0f, 0.0f, 0.0f, 1.0f),
                .UV = vec2(cosPhi * 0.5f + 0.5f, sinPhi * 0.5f + 0.5f),
            });
        }
        for (u32 s = 0; s < segments; ++s)
        {
            data.Indices.push_back(center);
            data.Indices.push_back(rimBase + s);
            data.Indices.push_back(rimBase + s + 1);
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Torus(f32 majorRadius, f32 minorRadius, u32 majorSegments, u32 minorSegments,
                   AssetHandle<MaterialInstance> material)
    {
        majorSegments = std::max(3u, majorSegments);
        minorSegments = std::max(3u, minorSegments);

        constexpr f32 Pi = 3.14159265358979323846f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(majorSegments + 1) * (minorSegments + 1));
        data.Indices.reserve(static_cast<usize>(majorSegments) * minorSegments * 6);

        // (majorSegments+1) columns around the ring x (minorSegments+1) columns
        // around the tube. The seam columns duplicate so UVs do not wrap.
        for (u32 i = 0; i <= majorSegments; ++i)
        {
            const f32 u = static_cast<f32>(i) / static_cast<f32>(majorSegments);
            const f32 theta = u * 2.0f * Pi;
            const f32 cosTheta = std::cos(theta);
            const f32 sinTheta = std::sin(theta);

            // Center of the tube cross-section at this major angle.
            const vec3 center = vec3(cosTheta * majorRadius, 0.0f, sinTheta * majorRadius);

            for (u32 j = 0; j <= minorSegments; ++j)
            {
                const f32 v = static_cast<f32>(j) / static_cast<f32>(minorSegments);
                const f32 phi = v * 2.0f * Pi;
                const f32 cosPhi = std::cos(phi);
                const f32 sinPhi = std::sin(phi);

                const vec3 normal = vec3(cosTheta * cosPhi, sinPhi, sinTheta * cosPhi);

                // +U direction (increasing major angle), tangent to the ring.
                const vec3 tangent = vec3(-sinTheta, 0.0f, cosTheta);

                data.Vertices.push_back(CanonicalVertex{
                    .Position = center + normal * minorRadius,
                    .Normal = normal,
                    .Tangent = vec4(tangent, 1.0f),
                    .UV = vec2(u, v),
                });
            }
        }

        const u32 stride = minorSegments + 1;
        for (u32 i = 0; i < majorSegments; ++i)
        {
            for (u32 j = 0; j < minorSegments; ++j)
            {
                const u32 a = i * stride + j;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // CCW seen from outside the tube.
                data.Indices.push_back(a);
                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(b);
                data.Indices.push_back(d);
                data.Indices.push_back(c);
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }

    MeshData Annulus(f32 innerRadius, f32 outerRadius, u32 radialSegments, u32 angularSegments,
                     u32 angularSubmeshes, AssetHandle<MaterialInstance> material)
    {
        radialSegments = std::max(1u, radialSegments);
        angularSegments = std::max(3u, angularSegments);
        angularSubmeshes = std::max(1u, angularSubmeshes);

        // Equal sectors: raise the column count to the next multiple so every sector spans the
        // same arc and no quad straddles a boundary.
        if (const u32 remainder = angularSegments % angularSubmeshes; remainder != 0)
        {
            angularSegments += angularSubmeshes - remainder;
        }

        if (innerRadius > outerRadius)
        {
            std::swap(innerRadius, outerRadius);
        }

        constexpr f32 Pi = 3.14159265358979323846f;

        MeshData data;
        data.Vertices.reserve(static_cast<usize>(angularSegments + 1) * (radialSegments + 1));
        data.Indices.reserve(static_cast<usize>(angularSegments) * radialSegments * 6);

        // (angularSegments+1) columns around the ring x (radialSegments+1) rows across it, laid
        // out angular-major so each sector's quads form one contiguous index range. The seam
        // columns duplicate so UVs do not wrap.
        for (u32 j = 0; j <= angularSegments; ++j)
        {
            const f32 v = static_cast<f32>(j) / static_cast<f32>(angularSegments);
            const f32 theta = v * 2.0f * Pi;
            const f32 cosTheta = std::cos(theta);
            const f32 sinTheta = std::sin(theta);

            for (u32 i = 0; i <= radialSegments; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(radialSegments);
                const f32 radius = innerRadius + (outerRadius - innerRadius) * u;

                data.Vertices.push_back(CanonicalVertex{
                    .Position = vec3(cosTheta * radius, 0.0f, sinTheta * radius),
                    .Normal = vec3(0.0f, 1.0f, 0.0f),
                    // +U direction (increasing radius), pointing radially outward.
                    .Tangent = vec4(cosTheta, 0.0f, sinTheta, 1.0f),
                    .UV = vec2(u, v),
                });
            }
        }

        const u32 stride = radialSegments + 1;
        for (u32 j = 0; j < angularSegments; ++j)
        {
            for (u32 i = 0; i < radialSegments; ++i)
            {
                const u32 a = j * stride + i;
                const u32 b = a + 1;      // one step outward
                const u32 c = a + stride; // one step around
                const u32 d = c + 1;

                // CCW seen from above (+Y looking down -Y), matching Plane. Radial-then-angular
                // order inverts one triangle per quad, so the angular step leads both.
                data.Indices.push_back(a);
                data.Indices.push_back(c);
                data.Indices.push_back(b);

                data.Indices.push_back(b);
                data.Indices.push_back(c);
                data.Indices.push_back(d);
            }
        }

        // The sectors share one vertex grid and one material; only the index range differs. The
        // column count is a multiple of the sector count, so the division is exact.
        const u32 materialIndex = RecordMaterial(data, std::move(material));
        const u32 indicesPerSector = static_cast<u32>(data.Indices.size()) / angularSubmeshes;
        for (u32 sector = 0; sector < angularSubmeshes; ++sector)
        {
            data.SubMeshes.push_back(SubMesh{
                .IndexOffset = sector * indicesPerSector,
                .IndexCount = indicesPerSector,
                .MaterialIndex = materialIndex,
            });
        }

        return data;
    }

    MeshData Capsule(f32 radius, f32 height, u32 segments, u32 rings,
                     AssetHandle<MaterialInstance> material)
    {
        segments = std::max(3u, segments);
        rings = std::max(1u, rings);

        constexpr f32 Pi = 3.14159265358979323846f;
        const f32 halfH = height * 0.5f;

        MeshData data;
        // Latitude rows: (rings+1) for the top hemisphere, (rings+1) for the
        // bottom, sharing the two equator rows at ±halfH (one row each).
        const u32 latRows = 2 * (rings + 1);
        data.Vertices.reserve(static_cast<usize>(latRows) * (segments + 1));
        data.Indices.reserve(static_cast<usize>(latRows - 1) * segments * 6);

        // Build rows top to bottom. The top hemisphere sweeps the polar angle
        // 0..pi/2 with the ring center at +halfH; the bottom sweeps pi/2..pi with
        // the center at -halfH. Each row's vertices ride the segment columns.
        auto appendRow = [&](f32 polar, f32 centerY)
        {
            const f32 cosPolar = std::cos(polar);
            const f32 sinPolar = std::sin(polar);
            const f32 vt = polar / Pi;

            for (u32 s = 0; s <= segments; ++s)
            {
                const f32 vu = static_cast<f32>(s) / static_cast<f32>(segments);
                const f32 phi = vu * 2.0f * Pi;
                const f32 cosPhi = std::cos(phi);
                const f32 sinPhi = std::sin(phi);

                const vec3 normal = vec3(sinPolar * cosPhi, cosPolar, sinPolar * sinPhi);

                vec3 tangent = vec3(-sinPhi, 0.0f, cosPhi);
                if (sinPolar <= 1e-6f)
                {
                    tangent = vec3(1.0f, 0.0f, 0.0f);
                }

                data.Vertices.push_back(CanonicalVertex{
                    .Position = normal * radius + vec3(0.0f, centerY, 0.0f),
                    .Normal = normal,
                    .Tangent = vec4(glm::normalize(tangent), 1.0f),
                    .UV = vec2(vu, vt),
                });
            }
        };

        // Top hemisphere: polar 0 (north pole) down to pi/2 (equator) at +halfH.
        for (u32 r = 0; r <= rings; ++r)
        {
            const f32 polar = static_cast<f32>(r) / static_cast<f32>(rings) * (Pi * 0.5f);
            appendRow(polar, +halfH);
        }
        // Bottom hemisphere: polar pi/2 (equator) down to pi (south pole) at -halfH.
        for (u32 r = 0; r <= rings; ++r)
        {
            const f32 polar =
                (Pi * 0.5f) + static_cast<f32>(r) / static_cast<f32>(rings) * (Pi * 0.5f);
            appendRow(polar, -halfH);
        }

        const u32 stride = segments + 1;
        for (u32 row = 0; row < latRows - 1; ++row)
        {
            for (u32 s = 0; s < segments; ++s)
            {
                const u32 a = row * stride + s;
                const u32 b = a + 1;
                const u32 c = a + stride;
                const u32 d = c + 1;

                // Skip the degenerate triangle at each pole row.
                if (row != 0)
                {
                    data.Indices.push_back(a);
                    data.Indices.push_back(b);
                    data.Indices.push_back(c);
                }
                if (row != latRows - 2)
                {
                    data.Indices.push_back(b);
                    data.Indices.push_back(d);
                    data.Indices.push_back(c);
                }
            }
        }

        FinishSubMesh(data, std::move(material));
        return data;
    }
}
