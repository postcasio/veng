#include "SphereTerrainSelection.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    namespace
    {
        // The largest tile-edge arc at level L is at most pi / 2^(L+1) radians: a level-L tile spans
        // pi / 2^(L+1) of warp angle, and the arc it spans is longest along a face's centre lines.
        f64 LevelArc(const u32 level)
        {
            return std::ldexp(std::numbers::pi / 2.0, -static_cast<i32>(level));
        }

        struct HeightRange
        {
            f32 Min = 0.0f;
            f32 Max = 0.0f;
        };

        // A visited tile's working bound and placement, kept to emit the drawn set after balance.
        struct Node
        {
            HeightRange Working;
            HeightRange Own;
            u32 Layer = 0;
        };

        struct Walker
        {
            const SphereTerrainSelectionInput& In;
            SphereTerrainSelectionResult& Out;
            std::unordered_map<u64, Node> Nodes;
            std::unordered_set<u64> Blocked;
            f64 RangeAtZero = 0.0;
            f64 Occluder = 0.0;

            [[nodiscard]] HeightRange Clamp(HeightRange range) const
            {
                range.Min = std::max(range.Min, In.MinHeight);
                range.Max = std::min(range.Max, In.MaxHeight);
                range.Max = std::max(range.Max, range.Min);
                return range;
            }

            [[nodiscard]] bool IsInFrustum(const SphereTerrainTileBox& box) const
            {
                if (In.Frustum == nullptr)
                {
                    return true;
                }
                const std::array<dvec3, 8> corners = SphereTerrainBoxCorners(box);
                for (const glm::dvec4& plane : *In.Frustum)
                {
                    bool anyInside = false;
                    for (const dvec3& corner : corners)
                    {
                        if (glm::dot(dvec3(plane), corner - In.Eye) + plane.w >= 0.0)
                        {
                            anyInside = true;
                            break;
                        }
                    }
                    if (!anyInside)
                    {
                        return false;
                    }
                }
                return true;
            }

            [[nodiscard]] bool IsChildResident(const CubeSphereTileKey child) const
            {
                const SphereTerrainTileRecord* record = In.Tiles->Find(child);
                return record != nullptr && record->State == SphereTerrainTileState::Resident;
            }

            void WantOrRequest(const CubeSphereTileKey child, const SphereTerrainRequestKind kind,
                               const f32 urgency, const bool inFrustum)
            {
                if (IsChildResident(child))
                {
                    Out.Wanted.push_back(child);
                    return;
                }
                Out.Requests.push_back(
                    {.Key = child, .Kind = kind, .Urgency = urgency, .InFrustum = inFrustum});
            }

            void Visit(const CubeSphereTileKey key, const optional<HeightRange>& parentOwn)
            {
                const SphereTerrainTileRecord* record = In.Tiles->Find(key);
                VE_ASSERT(record != nullptr && record->State == SphereTerrainTileState::Resident,
                          "sphere terrain selection visited a tile that is not resident");
                const u32 level = key.GetLevel();
                const SphereTerrainSelectionSettings& settings = In.Settings;
                const f64 h = settings.Hysteresis;
                Out.Stats.Visited++;
                Out.Wanted.push_back(key);

                HeightRange own{.Min = In.MinHeight, .Max = In.MaxHeight};
                if (record->Bound.Measured)
                {
                    own = {.Min = record->Bound.Min, .Max = record->Bound.Max};
                }
                else if (parentOwn.has_value())
                {
                    const f32 widen = In.LevelHeightBounds[level];
                    own = Clamp({.Min = parentOwn->Min - widen, .Max = parentOwn->Max + widen});
                }
                HeightRange working = own;
                if (parentOwn.has_value())
                {
                    working.Min = std::min(working.Min, parentOwn->Min);
                    working.Max = std::max(working.Max, parentOwn->Max);
                }
                working = Clamp(working);
                Nodes[key.Value] = {.Working = working, .Own = own, .Layer = record->Layer};

                const SphereTerrainTileBox box =
                    ComputeSphereTerrainTileBox(key, In.Radius, working.Min, working.Max);
                const f64 distance = SphereTerrainBoxDistance(box, In.Eye);
                const f64 range = std::ldexp(RangeAtZero, -static_cast<i32>(level));
                const bool wasSplit =
                    In.PreviousSplits != nullptr && In.PreviousSplits->contains(key.Value);
                const bool below = IsBoxBelowHorizon(box, In.Eye, Occluder);
                const bool canDeepen = level < In.MaxLevel && !below;

                const bool distanceWants =
                    wasSplit ? distance <= (1.0 + h) * range : distance < (1.0 - h) * range;
                f64 projected = 0.0;
                bool heightWants = false;
                if (record->Bound.Measured && settings.TargetHeightErrorPixels > 0.0f)
                {
                    projected =
                        record->Bound.MorphError * In.PixelsPerRadian / std::max(distance, 1e-6);
                    const f64 target = settings.TargetHeightErrorPixels;
                    heightWants =
                        wasSplit ? projected > target * (1.0 - h) : projected > target * (1.0 + h);
                }

                if (canDeepen && (distanceWants || heightWants))
                {
                    bool allResident = true;
                    for (u32 child = 0; child < 4; child++)
                    {
                        allResident = allResident && IsChildResident(key.GetChild(child));
                    }
                    if (allResident)
                    {
                        Out.Splits.insert(key.Value);
                        for (u32 child = 0; child < 4; child++)
                        {
                            Visit(key.GetChild(child), own);
                        }
                        return;
                    }
                    Out.Stats.WaitingSplits++;
                    const f64 urgency =
                        std::max((1.0 - h) * range / std::max(distance, 1e-3),
                                 projected / std::max<f64>(settings.TargetHeightErrorPixels, 1e-6));
                    const bool inFrustum = IsInFrustum(box);
                    for (u32 child = 0; child < 4; child++)
                    {
                        WantOrRequest(key.GetChild(child), SphereTerrainRequestKind::Split,
                                      static_cast<f32>(urgency), inFrustum);
                    }
                    return;
                }

                if (canDeepen && distance < settings.PrefetchRangeScale * (1.0 - h) * range)
                {
                    const bool inFrustum = IsInFrustum(box);
                    for (u32 child = 0; child < 4; child++)
                    {
                        WantOrRequest(key.GetChild(child), SphereTerrainRequestKind::Prefetch, 0.0f,
                                      inFrustum);
                    }
                }
            }

            // Splits a tile the distance rule left whole because a split neighbour needs it: past
            // the horizon the rule stops refining, and the drawn set must still be balanced there.
            // Fails when the tile, or an ancestor it needs, has a child still generating.
            bool ForceSplit(const CubeSphereTileKey key)
            {
                if (Out.Splits.contains(key.Value))
                {
                    return true;
                }
                if (Blocked.contains(key.Value) || key.GetLevel() >= In.MaxLevel)
                {
                    return false;
                }
                if (!Nodes.contains(key.Value) &&
                    (key.GetLevel() == 0 || !ForceSplit(key.GetParent()) ||
                     !Nodes.contains(key.Value)))
                {
                    return false;
                }
                for (u32 child = 0; child < 4; child++)
                {
                    if (!IsChildResident(key.GetChild(child)))
                    {
                        return false;
                    }
                }
                Out.Splits.insert(key.Value);
                Out.Stats.SplitForBalance++;
                const HeightRange own = Nodes.at(key.Value).Own;
                for (u32 child = 0; child < 4; child++)
                {
                    if (!Nodes.contains(key.GetChild(child).Value))
                    {
                        Visit(key.GetChild(child), own);
                    }
                }
                return true;
            }

            // Makes the split set 2:1 balanced: every split tile's same-level neighbours must exist,
            // which is exactly when no drawn tile meets one more than a level finer. A missing
            // neighbour's parent is split when its children are resident; otherwise the finer
            // tile's split is undone until they land, keeping its children wanted.
            void Balance()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    vector<u64> splits(Out.Splits.begin(), Out.Splits.end());
                    std::ranges::sort(splits,
                                      [](const u64 a, const u64 b)
                                      {
                                          const u32 la = CubeSphereTileKey::FromValue(a).GetLevel();
                                          const u32 lb = CubeSphereTileKey::FromValue(b).GetLevel();
                                          return la != lb ? la > lb : a < b;
                                      });
                    for (const u64 value : splits)
                    {
                        const CubeSphereTileKey key = CubeSphereTileKey::FromValue(value);
                        if (key.GetLevel() == 0 || !Out.Splits.contains(value))
                        {
                            continue;
                        }
                        const usize before = Out.Splits.size();
                        bool balanced = true;
                        for (u32 edge = 0; edge < 4 && balanced; edge++)
                        {
                            const CubeSphereTileKey neighbour =
                                key.GetNeighbour(static_cast<CubeSphere::Edge>(edge));
                            balanced = ForceSplit(neighbour.GetParent());
                        }
                        changed = changed || Out.Splits.size() != before;
                        if (balanced)
                        {
                            continue;
                        }
                        std::erase_if(Out.Splits,
                                      [key](const u64 other)
                                      {
                                          return other == key.Value ||
                                                 key.IsAncestorOf(
                                                     CubeSphereTileKey::FromValue(other));
                                      });
                        Blocked.insert(key.Value);
                        Out.Stats.HeldForBalance++;
                        changed = true;
                    }
                }
            }

            void Emit(const CubeSphereTileKey key)
            {
                if (Out.Splits.contains(key.Value))
                {
                    for (u32 child = 0; child < 4; child++)
                    {
                        Emit(key.GetChild(child));
                    }
                    return;
                }
                const auto node = Nodes.find(key.Value);
                if (node == Nodes.end())
                {
                    return;
                }
                const u32 level = key.GetLevel();
                const f64 h = In.Settings.Hysteresis;
                const f64 morphEnd =
                    2.0 * (1.0 - h) * std::ldexp(RangeAtZero, -static_cast<i32>(level));
                u32 parentLayer = SphereTerrainNoLayer;
                if (level > 0)
                {
                    if (const auto parent = Nodes.find(key.GetParent().Value);
                        parent != Nodes.end())
                    {
                        parentLayer = parent->second.Layer;
                    }
                }
                Out.Drawn.push_back({
                    .Key = key,
                    .Layer = node->second.Layer,
                    .ParentLayer = parentLayer,
                    .MinHeight = node->second.Working.Min,
                    .MaxHeight = node->second.Working.Max,
                    .MorphStart = static_cast<f32>(In.Settings.MorphStartRatio * morphEnd),
                    .MorphEnd = static_cast<f32>(morphEnd),
                });
                Out.Stats.DrawnTiles++;
                Out.Stats.DrawnPerLevel[level]++;
            }
        };
    }

    f64 SphereTerrainLevelSpacing(const f64 radius, const u32 level)
    {
        return radius * LevelArc(level) / static_cast<f64>(SphereTerrainGridQuads);
    }

    f64 SphereTerrainLevelDiameter(const f64 radius, const u32 level)
    {
        return radius * std::numbers::sqrt2 * LevelArc(level);
    }

    f64 SphereTerrainLevelRange(const f64 radius, const f64 pixelsPerRadian,
                                const f32 targetPixelsPerQuad, const f32 planarRangeFloor,
                                const u32 level)
    {
        const f64 atZero =
            std::max(SphereTerrainLevelSpacing(radius, 0) * pixelsPerRadian / targetPixelsPerQuad,
                     planarRangeFloor * SphereTerrainLevelDiameter(radius, 0));
        return std::ldexp(atZero, -static_cast<i32>(level));
    }

    SphereTerrainTileBox ComputeSphereTerrainTileBox(const CubeSphereTileKey key, const f64 radius,
                                                     const f32 minHeight, const f32 maxHeight)
    {
        const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(key);
        const std::array<dvec3, 4>& c = geometry.Corners;
        const std::array<dvec3, 9> samples = {
            c[0],
            c[1],
            c[2],
            c[3],
            glm::normalize(c[0] + c[1]),
            glm::normalize(c[2] + c[3]),
            glm::normalize(c[0] + c[2]),
            glm::normalize(c[1] + c[3]),
            geometry.Center,
        };

        SphereTerrainTileBox box;
        const dvec3 up = geometry.Center;
        dvec3 across = (c[1] + c[3]) - (c[0] + c[2]);
        across = glm::normalize(across - (glm::dot(across, up) * up));
        box.Axes = {across, glm::cross(up, across), up};
        box.Origin = up * radius;

        const f64 low = radius + minHeight;
        const f64 high = radius + maxHeight;
        box.Min = dvec3(std::numeric_limits<f64>::max());
        box.Max = dvec3(std::numeric_limits<f64>::lowest());
        for (const dvec3& direction : samples)
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

        // Between samples the sphere bulges past the chords joining them by at most the sagitta of
        // the widest gap: half a sample spacing along the diagonal.
        const f64 halfGap = std::numbers::sqrt2 * geometry.ThetaSpan / 8.0;
        const f64 sagitta = std::max(high, low) * 2.0 * std::sin(halfGap) * std::sin(halfGap);
        box.Min -= dvec3(sagitta);
        box.Max += dvec3(sagitta);
        return box;
    }

    f64 SphereTerrainBoxDistance(const SphereTerrainTileBox& box, const dvec3& point)
    {
        const dvec3 offset = point - box.Origin;
        const dvec3 local = {glm::dot(offset, box.Axes[0]), glm::dot(offset, box.Axes[1]),
                             glm::dot(offset, box.Axes[2])};
        return glm::length(local - glm::clamp(local, box.Min, box.Max));
    }

    std::array<dvec3, 8> SphereTerrainBoxCorners(const SphereTerrainTileBox& box)
    {
        std::array<dvec3, 8> corners{};
        for (u32 i = 0; i < 8; i++)
        {
            const dvec3 local = {(i & 1u) != 0 ? box.Max.x : box.Min.x,
                                 (i & 2u) != 0 ? box.Max.y : box.Min.y,
                                 (i & 4u) != 0 ? box.Max.z : box.Min.z};
            corners[i] = box.Origin + (local.x * box.Axes[0]) + (local.y * box.Axes[1]) +
                         (local.z * box.Axes[2]);
        }
        return corners;
    }

    bool IsHiddenBySphere(const dvec3& eye, const dvec3& point, const f64 occluderRadius)
    {
        const f64 rr = occluderRadius * occluderRadius;
        if (glm::dot(eye, eye) <= rr)
        {
            return false;
        }
        if (glm::dot(point, point) < rr)
        {
            return true;
        }
        const dvec3 segment = point - eye;
        const f64 length2 = glm::dot(segment, segment);
        if (length2 <= 0.0)
        {
            return false;
        }
        const f64 t = std::clamp(-glm::dot(eye, segment) / length2, 0.0, 1.0);
        const dvec3 closest = eye + (t * segment);
        return glm::dot(closest, closest) < rr;
    }

    bool IsBoxBelowHorizon(const SphereTerrainTileBox& box, const dvec3& eye,
                           const f64 occluderRadius)
    {
        return std::ranges::all_of(SphereTerrainBoxCorners(box), [&](const dvec3& corner)
                                   { return IsHiddenBySphere(eye, corner, occluderRadius); });
    }

    SphereTerrainSelectionResult SelectSphereTerrain(const SphereTerrainSelectionInput& input)
    {
        VE_ASSERT(input.Tiles != nullptr, "sphere terrain selection needs the pool's tiles");
        SphereTerrainSelectionResult result;
        Walker walker{
            .In = input,
            .Out = result,
            .RangeAtZero = SphereTerrainLevelRange(input.Radius, input.PixelsPerRadian,
                                                   input.TargetPixelsPerQuad,
                                                   input.Settings.PlanarRangeFloor, 0),
            .Occluder = input.Radius + input.MinHeight,
        };

        for (u32 face = 0; face < CubeSphere::FaceCount; face++)
        {
            const CubeSphereTileKey root =
                CubeSphereTileKey::Make(static_cast<CubeSphere::Face>(face), 0, 0, 0);
            if (walker.IsChildResident(root))
            {
                walker.Visit(root, std::nullopt);
            }
            else
            {
                result.Requests.push_back({.Key = root, .Urgency = 1e30f});
            }
        }

        walker.Balance();
        for (u32 face = 0; face < CubeSphere::FaceCount; face++)
        {
            walker.Emit(CubeSphereTileKey::Make(static_cast<CubeSphere::Face>(face), 0, 0, 0));
        }

        std::ranges::stable_sort(
            result.Requests,
            [](const SphereTerrainSelectionRequest& a, const SphereTerrainSelectionRequest& b)
            {
                if (a.Kind != b.Kind)
                {
                    return a.Kind == SphereTerrainRequestKind::Split;
                }
                if (a.InFrustum != b.InFrustum)
                {
                    return a.InFrustum;
                }
                return a.Urgency > b.Urgency;
            });
        result.Stats.Requests = static_cast<u32>(result.Requests.size());
        return result;
    }
}
