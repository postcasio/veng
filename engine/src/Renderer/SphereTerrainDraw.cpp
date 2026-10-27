#include "SphereTerrainDraw.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Veng/Math/CubeSphere.h>

#include "SphereTerrainSelection.h"

namespace Veng::Renderer
{
    namespace
    {
        bool IsOutside(const SphereTerrainDrawView& view, const std::array<dvec3, 8>& corners,
                       const dvec3& eye)
        {
            for (const glm::dvec4& plane : view.Planes)
            {
                const bool allOutside = std::ranges::all_of(
                    corners, [&](const dvec3& corner)
                    { return glm::dot(dvec3(plane), corner - eye) + plane.w < 0.0; });
                if (allOutside)
                {
                    return true;
                }
            }
            return false;
        }

        vec4 Point(const dvec3& p)
        {
            return {vec3(p), 0.0f};
        }

        // The f32 rounding step at a magnitude.
        f64 Ulp(const f64 magnitude)
        {
            return std::ldexp(std::max(magnitude, 1.0), -23);
        }

        // The deepest step a tile's skirt must cover. A finer neighbour meets a tile partly
        // morphed only where the coarse side was held, and its edge then stands off the coarse
        // edge by at most the finer tile's MorphError, so a tile covers its own and its finer
        // neighbours' — the next level's largest — plus the parent grid's chord sagitta, the f32
        // rounding of a position at the tile's distance, and two tiles' rounding of one texel.
        f32 SkirtDepth(const SphereTerrainDrawnTile& tile, const std::array<f32, 26>& levelError,
                       const f64 farthest, const f64 radius, const f64 edge, const f32 agreement)
        {
            const u32 level = tile.Key.GetLevel();
            const f64 step = std::max(levelError[level], levelError[level + 1]);
            const f64 parentSpacing = 2.0 * edge / SphereTerrainGridQuads;
            const f64 sagitta = parentSpacing * parentSpacing / (8.0 * radius);
            return std::max(static_cast<f32>(step + sagitta + (2.0 * Ulp(farthest)) + agreement),
                            SphereTerrainMinSkirtMetres);
        }
    }

    SphereTerrainFrame BuildSphereTerrainFrame(const SphereTerrainFrameInput& input)
    {
        SphereTerrainFrame frame;
        const bool shadows = input.CastsShadows && input.Scale == 1.0f;
        const u32 viewCount = 1 + (shadows ? static_cast<u32>(input.Shadows.size()) : 0u);
        frame.Lists.resize(1 + input.Shadows.size());
        const u32 layerCount = std::min(input.LayerCount, MaxSphereTerrainLevelLayers);

        std::array<f32, 26> levelError{};
        for (const SphereTerrainDrawnTile& tile : input.Drawn)
        {
            f32& error = levelError[tile.Key.GetLevel()];
            error = std::max(error, tile.MorphError);
        }
        const f32 agreement = static_cast<f32>(
            std::ldexp(std::max(std::abs(input.MinHeight), std::abs(input.MaxHeight)), -21));
        const f64 occluder = input.Radius + input.MinHeight;

        for (const SphereTerrainDrawnTile& tile : input.Drawn)
        {
            f32 low = tile.MinHeight;
            f32 high = tile.MaxHeight;
            for (u32 layer = 0; layer < layerCount; layer++)
            {
                low = std::min(low, input.LayerHeights[layer]);
                high = std::max(high, input.LayerHeights[layer]);
            }
            const SphereTerrainTileBox box =
                ComputeSphereTerrainTileBox(tile.Key, input.Radius, low, high);
            const std::array<dvec3, 8> corners = SphereTerrainBoxCorners(box);

            std::array<bool, 1 + MaxSphereTerrainLevelLayers> surfaces{};
            surfaces[0] = true;
            for (u32 layer = 0; layer < layerCount; layer++)
            {
                surfaces[1 + layer] = tile.MinHeight < input.LayerHeights[layer];
            }

            const u32 recordIndex = static_cast<u32>(frame.Records.size());
            bool drawn = false;
            for (u32 view = 0; view < viewCount; view++)
            {
                const SphereTerrainDrawView& cull =
                    view == 0 ? input.Main : input.Shadows[view - 1];
                if (IsOutside(cull, corners, input.Eye))
                {
                    frame.CulledByFrustum += view == 0 ? 1u : 0u;
                    continue;
                }
                if (cull.Horizon && IsBoxBelowHorizon(box, input.Eye, occluder))
                {
                    frame.CulledByHorizon += view == 0 ? 1u : 0u;
                    continue;
                }
                for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
                {
                    if (surfaces[surface])
                    {
                        frame.Lists[view][surface].push_back(recordIndex);
                    }
                }
                drawn = true;
            }
            if (!drawn)
            {
                continue;
            }

            const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(tile.Key);
            SphereTerrainDrawRecord& record = frame.Records.emplace_back();
            record.Tile = PackSphereTerrainTileConstants(tile.Key, input.Radius);
            for (u32 i = 0; i < 4; i++)
            {
                const dvec3 offset = input.Radius * geometry.Corners[i];
                record.Corners[i] = Point(input.CenterFromEye + (input.Orientation * offset));
                record.Verticals[i] = Point(input.Orientation * geometry.Corners[i]);
                record.PrevCorners[i] =
                    Point(input.PrevCenterFromCamera + (input.PrevOrientation * offset));
            }
            const auto Chord = [&](const u32 a, const u32 b)
            {
                const dvec3 d = geometry.Corners[a] - geometry.Corners[b];
                return static_cast<f32>(glm::dot(d, d));
            };
            record.ChordsA = {Chord(0, 1), Chord(0, 2), Chord(0, 3), Chord(1, 2)};
            record.ChordsB = {Chord(1, 3), Chord(2, 3), static_cast<f32>(input.Radius),
                              input.Scale};
            record.TangentU = Point(input.Orientation *
                                    glm::normalize((geometry.Corners[1] - geometry.Corners[0]) +
                                                   (geometry.Corners[3] - geometry.Corners[2])));

            const bool hasParent =
                tile.Key.GetLevel() > 0 && tile.ParentLayer != SphereTerrainNoLayer;
            const f32 floor = [&]
            {
                if (input.ArrivalFloors == nullptr)
                {
                    return 0.0f;
                }
                const auto it = input.ArrivalFloors->find(tile.Key.Value);
                return it != input.ArrivalFloors->end() ? it->second : 0.0f;
            }();
            const f64 edge = input.Radius * std::sqrt(static_cast<f64>(Chord(0, 1)));
            const f64 farthest =
                glm::length(input.CenterFromEye +
                            (input.Orientation * (input.Radius * geometry.Center))) +
                (2.0 * edge) + std::abs(input.MaxHeight);
            constexpr f32 Never = std::numeric_limits<f32>::max();
            record.Morph = {hasParent ? tile.MorphStart : Never, hasParent ? tile.MorphEnd : Never,
                            hasParent ? floor : 0.0f,
                            SkirtDepth(tile, levelError, farthest, input.Radius, edge, agreement)};
            record.Heights = {input.LayerHeights[0], input.LayerHeights[1],
                              static_cast<f32>(edge / SphereTerrainTileCells), 0.0f};
            record.Layers = {tile.Layer, hasParent ? tile.ParentLayer : tile.Layer,
                             hasParent ? tile.Key.GetQuadrant() : 0u, hasParent ? 1u : 0u};
            if (input.TileData)
            {
                input.TileData(tile.Key, record.Data);
                if (hasParent)
                {
                    input.TileData(tile.Key.GetParent(), record.ParentData);
                }
            }
        }
        return frame;
    }
}
