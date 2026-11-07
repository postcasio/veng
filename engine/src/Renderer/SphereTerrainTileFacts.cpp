#include "SphereTerrainTileFacts.h"

#include <cmath>
#include <limits>
#include <numbers>

namespace Veng::Renderer
{
    SphereTerrainTileFrame MakeSphereTerrainTileFrame(const CubeSphereTileGeometry& geometry)
    {
        const std::array<dvec3, 4>& c = geometry.Corners;
        SphereTerrainTileFrame frame{
            .Samples =
                {
                    c[0],
                    c[1],
                    c[2],
                    c[3],
                    glm::normalize(c[0] + c[1]),
                    glm::normalize(c[2] + c[3]),
                    glm::normalize(c[0] + c[2]),
                    glm::normalize(c[1] + c[3]),
                    geometry.Center,
                },
        };
        const dvec3 up = geometry.Center;
        dvec3 across = (c[1] + c[3]) - (c[0] + c[2]);
        across = glm::normalize(across - (glm::dot(across, up) * up));
        frame.Axes = {across, glm::cross(up, across), up};
        // Between samples the sphere bulges past the chords joining them by at most the sagitta of
        // the widest gap: half a sample spacing along the diagonal.
        const f64 halfGap = std::numbers::sqrt2 * geometry.ThetaSpan / 8.0;
        frame.SagittaFactor = 2.0 * std::sin(halfGap) * std::sin(halfGap);
        return frame;
    }

    SphereTerrainTileBox ComputeSphereTerrainTileBox(const SphereTerrainTileFrame& frame,
                                                     const f64 radius, const f32 minHeight,
                                                     const f32 maxHeight)
    {
        SphereTerrainTileBox box;
        box.Axes = frame.Axes;
        box.Origin = frame.Axes[2] * radius;

        const f64 low = radius + minHeight;
        const f64 high = radius + maxHeight;
        box.Min = dvec3(std::numeric_limits<f64>::max());
        box.Max = dvec3(std::numeric_limits<f64>::lowest());
        for (const dvec3& direction : frame.Samples)
        {
            for (const f64 r : {low, high})
            {
                const dvec3 offset = (direction * r) - box.Origin;
                const dvec3 local = {glm::dot(offset, box.Axes[0]), glm::dot(offset, box.Axes[1]),
                                     glm::dot(offset, box.Axes[2])};
                box.Min = glm::min(box.Min, local);
                box.Max = glm::max(box.Max, local);
            }
        }
        const f64 sagitta = std::max(high, low) * frame.SagittaFactor;
        box.Min -= dvec3(sagitta);
        box.Max += dvec3(sagitta);
        return box;
    }

    SphereTerrainTileFacts MakeSphereTerrainTileFacts(const CubeSphereTileKey key, const f64 radius)
    {
        SphereTerrainTileFacts facts{.Geometry = GetCubeSphereTileGeometry(key)};
        const CubeSphereTileGeometry& geometry = facts.Geometry;
        facts.Frame = MakeSphereTerrainTileFrame(geometry);
        facts.Constants = PackSphereTerrainTileConstants(key, geometry, radius);
        const vec4& warp = facts.Constants.Warp;
        const vec2 arcU = CubeSphere::TileArcF32(warp.x, warp.y);
        const vec2 arcV = CubeSphere::TileArcF32(warp.z, warp.w);
        facts.Arc = {arcU, arcV};
        const auto Chord = [&](const u32 a, const u32 b)
        {
            const dvec3 d = geometry.Corners[a] - geometry.Corners[b];
            return static_cast<f32>(glm::dot(d, d));
        };
        facts.ChordsA = {Chord(0, 1), Chord(0, 2), Chord(0, 3), Chord(1, 2)};
        facts.ChordsB = {Chord(1, 3), Chord(2, 3)};
        facts.TangentU = glm::normalize((geometry.Corners[1] - geometry.Corners[0]) +
                                        (geometry.Corners[3] - geometry.Corners[2]));
        facts.Edge = radius * std::sqrt(static_cast<f64>(facts.ChordsA.x));
        return facts;
    }
}
