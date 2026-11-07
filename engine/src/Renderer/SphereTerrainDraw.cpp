#include "SphereTerrainDraw.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Veng/Assert.h>
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

        f64 FarthestCorner(const std::array<dvec3, 8>& corners, const dvec3& eye)
        {
            f64 farthest = 0.0;
            for (const dvec3& corner : corners)
            {
                farthest = std::max(farthest, glm::length(corner - eye));
            }
            return farthest;
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
        f32 SkirtDepth(const SphereTerrainDrawnTile& tile,
                       const std::array<f32, CubeSphere::MaxLevel + 2>& levelError,
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

    SphereTerrainMaterialVerdict CheckSphereTerrainMaterial(const MaterialDomain domain,
                                                            const bool sphereTerrain,
                                                            const bool levelLayer)
    {
        if (domain != MaterialDomain::Surface && domain != MaterialDomain::Translucent)
        {
            return SphereTerrainMaterialVerdict::WrongDomain;
        }
        if (!sphereTerrain)
        {
            return SphereTerrainMaterialVerdict::NotTerrain;
        }
        if (domain == MaterialDomain::Surface)
        {
            return SphereTerrainMaterialVerdict::Opaque;
        }
        return levelLayer ? SphereTerrainMaterialVerdict::Translucent
                          : SphereTerrainMaterialVerdict::TranslucentGround;
    }

    f64 SphereTerrainMinPixelAngle(const mat4& projection, const f32 heightPixels)
    {
        // A perspective projection's w is the view's depth; anything else has no pixel angle.
        if (projection[2][3] == 0.0f || projection[0][0] == 0.0f || projection[1][1] == 0.0f ||
            heightPixels <= 0.0f)
        {
            return 0.0;
        }
        const f64 tanX = 1.0 / std::abs(static_cast<f64>(projection[0][0]));
        const f64 tanY = 1.0 / std::abs(static_cast<f64>(projection[1][1]));
        const f64 pixelsPerRadian = 0.5 * static_cast<f64>(heightPixels) / tanY;
        return 1.0 / (pixelsPerRadian * (1.0 + (tanX * tanX) + (tanY * tanY)));
    }

    vec2 SphereTerrainCullHeights(const SphereTerrainDrawnTile& tile,
                                  const std::span<const f32> layerHeights)
    {
        f32 high = tile.MaxHeight;
        for (const f32 layer : layerHeights)
        {
            if (tile.MinHeight < layer)
            {
                high = std::max(high, layer);
            }
        }
        return {tile.MinHeight, high};
    }

    void CullSphereTerrainFrame(const SphereTerrainFrameInput& input, SphereTerrainFrame& frame)
    {
        const bool shadows = input.CastsShadows && input.Scale == 1.0f;
        const u32 viewCount = 1 + (shadows ? static_cast<u32>(input.Shadows.size()) : 0u);
        const u32 layerCount = std::min(input.LayerCount, MaxSphereTerrainLevelLayers);
        const std::span<const f32> layerHeights{input.LayerHeights.data(), layerCount};

        frame.Records.clear();
        frame.RecordTiles.clear();
        frame.RecordFacts.clear();
        frame.LocalFacts.clear();
        frame.Lists.resize(1 + input.Shadows.size());
        for (std::array<vector<u32>, SphereTerrainSurfaceCount>& view : frame.Lists)
        {
            for (vector<u32>& list : view)
            {
                list.clear();
            }
        }
        for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
        {
            frame.TranslucentLists[surface].clear();
            frame.NearLists[surface].clear();
            frame.NearTranslucentLists[surface].clear();
            frame.FarLists[surface].clear();
            frame.FarTranslucentLists[surface].clear();
        }
        frame.CulledByFrustum = 0;
        frame.CulledByHorizon = 0;
        frame.LevelError = {};
        for (const SphereTerrainDrawnTile& tile : input.Drawn)
        {
            f32& error = frame.LevelError[tile.Key.GetLevel()];
            error = std::max(error, tile.MorphError);
        }
        const f64 occluder = input.Radius + input.MinHeight;

        for (u32 index = 0; index < input.Drawn.size(); index++)
        {
            const SphereTerrainDrawnTile& tile = input.Drawn[index];
            const SphereTerrainTileRecord* held =
                input.Tiles != nullptr ? input.Tiles->Find(tile.Key) : nullptr;
            const SphereTerrainTileFacts* facts =
                held != nullptr && held->HasFacts ? &held->Facts : nullptr;
            const bool local = facts == nullptr;
            if (local)
            {
                facts = &frame.LocalFacts.emplace_back(
                    MakeSphereTerrainTileFacts(tile.Key, input.Radius));
            }

            const vec2 heights = SphereTerrainCullHeights(tile, layerHeights);
            const SphereTerrainTileBox box =
                ComputeSphereTerrainTileBox(facts->Frame, input.Radius, heights.x, heights.y);
            const std::array<dvec3, 8> corners = SphereTerrainBoxCorners(box);

            std::array<bool, SphereTerrainSurfaceCount> surfaces{};
            std::array<bool, SphereTerrainSurfaceCount> translucent{};
            surfaces[0] = true;
            for (u32 layer = 0; layer < layerCount; layer++)
            {
                surfaces[1 + layer] = tile.MinHeight < input.LayerHeights[layer];
                translucent[1 + layer] = input.TranslucentLayers[layer];
            }

            const u32 recordIndex = static_cast<u32>(frame.RecordTiles.size());
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
                if (cull.Horizon && IsBoxBelowHorizon(corners, input.Eye, occluder))
                {
                    frame.CulledByHorizon += view == 0 ? 1u : 0u;
                    continue;
                }
                // In drawn units, the farthest corner bounds every fragment's distance from above
                // and the nearest point from below; the latter, over the smallest angle a pixel
                // subtends, bounds every fragment's footprint from below.
                const f64 farthest =
                    view == 0 ? static_cast<f64>(input.Scale) * FarthestCorner(corners, input.Eye)
                              : 0.0;
                const f64 footprint = view == 0 ? static_cast<f64>(input.Scale) *
                                                      SphereTerrainBoxDistance(box, input.Eye) *
                                                      input.MinPixelAngle
                                                : 0.0;
                bool listed = false;
                for (u32 surface = 0; surface < SphereTerrainSurfaceCount; surface++)
                {
                    if (!surfaces[surface] || (translucent[surface] && view > 0))
                    {
                        continue;
                    }
                    const f32 nearDistance = input.NearDistances[surface];
                    const bool near = view == 0 && nearDistance > 0.0f &&
                                      farthest <= static_cast<f64>(nearDistance);
                    const f32 farFootprint = input.FarFootprints[surface];
                    const bool far = view == 0 && !near && farFootprint > 0.0f &&
                                     footprint > static_cast<f64>(farFootprint);
                    if (translucent[surface])
                    {
                        (near  ? frame.NearTranslucentLists[surface]
                         : far ? frame.FarTranslucentLists[surface]
                               : frame.TranslucentLists[surface])
                            .push_back(recordIndex);
                    }
                    else
                    {
                        (near  ? frame.NearLists[surface]
                         : far ? frame.FarLists[surface]
                               : frame.Lists[view][surface])
                            .push_back(recordIndex);
                    }
                    listed = true;
                }
                drawn = drawn || listed;
            }
            if (drawn)
            {
                frame.RecordTiles.push_back(index);
                // A local fact may still move as more are computed, so it is pointed at below.
                frame.RecordFacts.push_back(local ? nullptr : facts);
            }
            else if (local)
            {
                frame.LocalFacts.pop_back();
            }
        }
        usize next = 0;
        for (const SphereTerrainTileFacts*& facts : frame.RecordFacts)
        {
            facts = facts != nullptr ? facts : &frame.LocalFacts[next++];
        }
    }

    void WriteSphereTerrainRecords(const SphereTerrainFrameInput& input,
                                   const SphereTerrainFrame& frame,
                                   const std::span<SphereTerrainDrawRecord> out)
    {
        VE_ASSERT(out.size() == frame.RecordTiles.size(),
                  "sphere terrain: {} records written for {} culled", out.size(),
                  frame.RecordTiles.size());
        const f32 agreement = static_cast<f32>(
            std::ldexp(std::max(std::abs(input.MinHeight), std::abs(input.MaxHeight)), -21));
        constexpr std::array<u32, MaxSphereTerrainTileWords> NoWords{};
        constexpr f32 Never = std::numeric_limits<f32>::max();

        for (usize r = 0; r < out.size(); r++)
        {
            const SphereTerrainDrawnTile& tile = input.Drawn[frame.RecordTiles[r]];
            const SphereTerrainTileFacts& facts = *frame.RecordFacts[r];
            const CubeSphereTileGeometry& geometry = facts.Geometry;
            SphereTerrainDrawRecord& record = out[r];
            record.Tile = facts.Constants;
            for (u32 i = 0; i < 4; i++)
            {
                const dvec3 offset = input.Radius * geometry.Corners[i];
                record.Corners[i] = Point(input.CenterFromEye + (input.Orientation * offset));
                record.Verticals[i] = Point(input.Orientation * geometry.Corners[i]);
                record.PrevCorners[i] =
                    Point(input.PrevCenterFromCamera + (input.PrevOrientation * offset));
            }
            record.ChordsA = facts.ChordsA;
            record.ChordsB = {facts.ChordsB.x, facts.ChordsB.y, static_cast<f32>(input.Radius),
                              input.Scale};
            record.TangentU = Point(input.Orientation * facts.TangentU);

            const bool hasParent =
                tile.Key.GetLevel() > 0 && tile.ParentLayer != SphereTerrainNoLayer;
            f32 floor = 0.0f;
            if (input.ArrivalFloors != nullptr)
            {
                const auto it = input.ArrivalFloors->find(tile.Key.Value);
                floor = it != input.ArrivalFloors->end() ? it->second : 0.0f;
            }
            const f64 edge = facts.Edge;
            const f64 farthest =
                glm::length(input.CenterFromEye +
                            (input.Orientation * (input.Radius * geometry.Center))) +
                (2.0 * edge) + std::abs(input.MaxHeight);
            record.Morph = {
                hasParent ? tile.MorphStart : Never, hasParent ? tile.MorphEnd : Never,
                hasParent ? floor : 0.0f,
                SkirtDepth(tile, frame.LevelError, farthest, input.Radius, edge, agreement)};
            record.Heights = {input.LayerHeights[0], input.LayerHeights[1],
                              static_cast<f32>(edge / SphereTerrainTileCells), 0.0f};
            const u32 flags = (hasParent ? SphereTerrainRecordHasParent : 0u) |
                              (tile.Held ? SphereTerrainRecordHeld : 0u) |
                              (input.TileOverlay ? SphereTerrainRecordTileOverlay : 0u);
            record.Layers = {tile.Layer, hasParent ? tile.ParentLayer : tile.Layer,
                             hasParent ? tile.Key.GetQuadrant() : 0u, flags};
            record.Arc = facts.Arc;
            record.Data = facts.Words;
            const SphereTerrainTileRecord* parent = hasParent && input.Tiles != nullptr
                                                        ? input.Tiles->Find(tile.Key.GetParent())
                                                        : nullptr;
            record.ParentData =
                parent != nullptr && parent->HasFacts ? parent->Facts.Words : NoWords;
        }
    }

    SphereTerrainFrame BuildSphereTerrainFrame(const SphereTerrainFrameInput& input)
    {
        SphereTerrainFrame frame;
        CullSphereTerrainFrame(input, frame);
        frame.Records.resize(frame.RecordTiles.size());
        WriteSphereTerrainRecords(input, frame, frame.Records);
        return frame;
    }
}
