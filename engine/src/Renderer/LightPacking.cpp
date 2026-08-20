#include <Veng/Renderer/LightPacking.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace Veng::Renderer
{
    namespace
    {
        // The punctual shadow atlas tile grid: CubeFaceCount columns × MaxShadowedPunctual
        // rows. A shadowed light's slot s, face f maps to tile (column f, row s) — so a spot
        // uses tile (0, s) and a point uses the whole of row s.
        constexpr u32 PunctualAtlasColumns = CubeFaceCount;
        constexpr u32 PunctualAtlasRows = MaxShadowedPunctual;

        // An area light is shadowed as a spot-style caster aimed along its Direction; this wide
        // half-angle (radians, < π/2) covers the scene in front. PCSS in the lighting pass softens
        // the penumbra by the light's own size, so a large light casts a soft shadow.
        constexpr f32 AreaShadowCone = 1.3f;

        // An area light whose scene is smaller than this fraction of its distance is treated as
        // near-parallel: the direction to it barely diverges across the scene, so it is shadowed by
        // the parallel-projection cascade atlas (which fits resolution to the camera view) instead
        // of its single perspective tile (which spreads one tile over the whole scene). A closer
        // area light, where the direction genuinely diverges, keeps the perspective tile.
        constexpr f32 CascadeParallaxThreshold = 0.2f;

        // One gathered scene light: what the packing pass resolved from the entity, plus the
        // shadow arms the contribution ranking then assigned it. Gathered in scene iteration
        // order (which is the packing order), ranked separately.
        struct LightCandidate
        {
            /// @brief The scene's Light component; the scene is not mutated while these live.
            const Light* Source = nullptr;
            /// @brief The entity's resolved world matrix.
            mat4 World{1.0f};
            /// @brief The light's world position, the translation of World.
            vec3 WorldPos{0.0f};
            /// @brief True for the Rect/Sphere/Polygon shapes the LTC path shades.
            bool IsArea = false;
            /// @brief Estimated contribution to the frame; 0 for a light that declines shadows.
            f32 Contribution = 0.0f;
            /// @brief Cascade set granted to this light, or -1.
            i32 CascadeSet = -1;
            /// @brief Punctual atlas slot granted to this light, or -1.
            i32 PunctualSlot = -1;
            /// @brief A shadow-casting directional the cascade budget could not seat.
            bool CascadeDenied = false;
        };

        // Rec.709 luminance: the single scalar the contribution ranking compares two lights'
        // colours by, so a saturated dim light does not outrank a bright white one on the
        // strength of its strongest channel.
        f32 Luminance(const vec3& color)
        {
            return glm::dot(color, vec3(0.2126f, 0.7152f, 0.0722f));
        }

        // A light's estimated contribution to the frame: the radiance the lighting pass would
        // apply at the point of the caster bound nearest the light — the brightest this light
        // can be anywhere in the drawn scene, which is the right question for "does it deserve
        // a shadow". The falloff is the shader's own: a Directional carries none, and everything
        // else takes the smooth range cutoff times the inverse square.
        //
        // The inverse square is clamped at its value one world unit out. Without it a light
        // standing inside the bound divides by ~0 and outranks every other by an arbitrary
        // factor; with it a co-located light ranks exactly as a directional of the same
        // radiance, which is the most it can honestly claim. An area light is scored the same
        // way even though its LTC integral carries the inverse square internally — the estimate
        // only has to order lights, and irradiance from a finite emitter falls the same way
        // past its own size.
        f32 EstimateContribution(const Light& light, const vec3& worldPos, const AABB& sceneBounds)
        {
            const f32 radiance = light.Intensity * Luminance(light.Color);
            if (radiance <= 0.0f)
            {
                return 0.0f;
            }
            if (light.Type == LightType::Directional)
            {
                return radiance;
            }

            const vec3 nearest = sceneBounds.IsEmpty()
                                     ? worldPos
                                     : glm::clamp(worldPos, sceneBounds.Min, sceneBounds.Max);
            const f32 distance = glm::length(nearest - worldPos);
            const f32 range = std::max(light.Range, 1e-4f);
            f32 rangeFactor = std::clamp(1.0f - std::pow(distance / range, 4.0f), 0.0f, 1.0f);
            rangeFactor *= rangeFactor;
            return radiance * rangeFactor / std::max(distance * distance, 1.0f);
        }

        // Whether an area light is near-parallel over the scene: the direction to it barely
        // diverges across the caster bound, so the parallel cascade projection — which fits its
        // resolution to the camera view — shadows the scene far better than the light's single
        // perspective tile spread over the whole of it.
        bool IsNearParallel(const Light& light, const vec3& worldPos, const AABB& sceneBounds)
        {
            if (sceneBounds.IsEmpty() || glm::length(light.Direction) <= 1e-5f)
            {
                return false;
            }
            const f32 distance = glm::length(sceneBounds.Center() - worldPos);
            const f32 extent = glm::length(sceneBounds.Size());
            return distance > 1e-3f && extent < distance * CascadeParallaxThreshold;
        }

        // The Cone.w flags word for a packed light: the two-sided bit, plus how the cascade arm
        // resolved — which set shadows the light, or that it asked for one and was denied.
        u32 PackLightFlags(const Light& light, const LightCandidate& candidate)
        {
            u32 flags = light.TwoSided ? LightFlags::TwoSided : 0u;
            if (candidate.CascadeSet >= 0)
            {
                // A Directional samples the cascade atlas by its type, so only an area light
                // needs a flag saying which arm shadows it.
                if (light.Type != LightType::Directional)
                {
                    flags |= LightFlags::AreaCascadeShadowed;
                }
                flags |= (static_cast<u32>(candidate.CascadeSet) << LightFlags::CascadeSetShift) &
                         LightFlags::CascadeSetMask;
            }
            else if (candidate.CascadeDenied)
            {
                flags |= LightFlags::CascadeDenied;
            }
            return flags;
        }

        // Bakes an atlas-tile remap into a punctual view-proj: a fragment projected by the
        // result lands in slot s, face f's tile, so the lighting pass samples the correct
        // tile by construction. NDC.xy in [-1,1] → the tile window; Z is left unchanged.
        // Built column-major to match glm.
        mat4 ComposePunctualTileRemap(const mat4& viewProj, const u32 slot, const u32 face)
        {
            const f32 sx = 1.0f / static_cast<f32>(PunctualAtlasColumns);
            const f32 sy = 1.0f / static_cast<f32>(PunctualAtlasRows);
            const f32 col = static_cast<f32>(face);
            const f32 row = static_cast<f32>(slot);

            mat4 remap(1.0f);
            remap[0][0] = sx;
            remap[1][1] = sy;
            remap[3][0] = sx * (2.0f * col + 1.0f) - 1.0f;
            remap[3][1] = sy * (2.0f * row + 1.0f) - 1.0f;
            return remap * viewProj;
        }
    }

    PackedSceneLights PackSceneLights(const Scene& world, const bool punctualShadows,
                                      const u32 punctualShadowResolution, const AABB& sceneBounds)
    {
        PackedSceneLights result;

        // Gather the scene's lights in iteration order — the order they are packed in below, and
        // the order the ranking falls back to on a tie.
        std::array<LightCandidate, SceneView::MaxLights> candidates;
        u32 candidateCount = 0;
        for (auto [entity, light] : world.View<Light>())
        {
            if (candidateCount >= SceneView::MaxLights)
            {
                break;
            }

            LightCandidate& candidate = candidates[candidateCount];
            candidate.Source = &light;
            candidate.World = WorldMatrix(world, entity);
            candidate.WorldPos = vec3(candidate.World[3]);
            candidate.IsArea = light.Type == LightType::Rect || light.Type == LightType::Sphere ||
                               light.Type == LightType::Polygon;
            // A light that declines shadows scores zero: it is passed over for every arm, since
            // aiming the scene's cascade at a light that then shades unshadowed would waste the
            // arm — which is the point of declining.
            candidate.Contribution =
                light.CastsShadows ? EstimateContribution(light, candidate.WorldPos, sceneBounds)
                                   : 0.0f;
            ++candidateCount;
        }

        // Rank by estimated contribution, descending. The sort is *stable*, which is what makes
        // an exact tie resolve to scene iteration order rather than to whatever the sort happened
        // to do — a total order that is the same every frame, so two equal lights cannot trade a
        // shadow between frames.
        std::array<u32, SceneView::MaxLights> ranked{};
        for (u32 i = 0; i < candidateCount; ++i)
        {
            ranked[i] = i;
        }
        const std::span<u32> byContribution(ranked.data(), candidateCount);
        std::ranges::stable_sort(
            byContribution, [&candidates](const u32 a, const u32 b)
            { return candidates[a].Contribution > candidates[b].Contribution; });

        // Spend the two shadow budgets from the top of the ranking down.
        for (const u32 index : byContribution)
        {
            LightCandidate& candidate = candidates[index];
            const Light& light = *candidate.Source;
            if (!light.CastsShadows)
            {
                continue;
            }

            // The cascade arm takes a Directional always, and an area light the parallax test
            // judges near-parallel. Each granted light gets a set of its own, fit to its own
            // direction, in its own band of the atlas.
            const bool isDirectional = light.Type == LightType::Directional;
            if (isDirectional ||
                (candidate.IsArea && IsNearParallel(light, candidate.WorldPos, sceneBounds)))
            {
                if (result.CascadeSetCount < MaxCascadeSets)
                {
                    const u32 set = result.CascadeSetCount;
                    candidate.CascadeSet = static_cast<i32>(set);
                    result.CascadeTravel[set] =
                        isDirectional ? light.Direction : glm::normalize(light.Direction);
                    ++result.CascadeSetCount;
                    continue;
                }
                if (isDirectional)
                {
                    // A parallel projection is the only shadow a directional has, so one past the
                    // budget is denied outright rather than aimed at another light's cascade. An
                    // area light falls through to its own perspective tile instead.
                    candidate.CascadeDenied = true;
                    ++result.DeniedDirectionalCount;
                    continue;
                }
            }

            // The punctual arm takes the highest-contributing MaxShadowedPunctual point/spot/area
            // lights that ask for one; the rest carry -1, as do all of them with punctual shadows
            // off. Point uses six cube faces; spot and area use a single perspective map (area
            // aimed along Direction, softened per-light-size by PCSS in the lighting pass).
            if (!punctualShadows || result.PunctualCount >= MaxShadowedPunctual ||
                !(light.Type == LightType::Point || light.Type == LightType::Spot ||
                  candidate.IsArea))
            {
                continue;
            }

            const u32 slot = result.PunctualCount;
            PunctualShadowRecord& record = result.PunctualRecords[slot];
            const f32 invResolution = 1.0f / static_cast<f32>(punctualShadowResolution);
            // Depth bias scales with world units per texel: a coarser tile needs more bias, a
            // finer one less. The shader adds a slope-scaled term on top. The floor/ceiling
            // clamp keeps a degenerate range from starving or flooding the compare.
            const auto texelBias = [](f32 worldPerTexel)
            { return std::clamp(worldPerTexel * 0.5f, 0.0005f, 0.01f); };
            if (light.Type != LightType::Point)
            {
                // Aim the perspective map along the light's travel direction; area lights use a
                // wide fixed cone as the cap, a spot its own outer cone. The scene bound then
                // tightens the frustum to the casters it must shadow.
                const f32 dirLen = glm::length(light.Direction);
                const vec3 aimDir =
                    dirLen > 1e-5f ? light.Direction / dirLen : vec3(0.0f, -1.0f, 0.0f);
                const f32 cone = candidate.IsArea ? AreaShadowCone : light.OuterCone;
                const std::optional<AABB> fitBounds =
                    sceneBounds.IsEmpty() ? std::nullopt : std::optional<AABB>(sceneBounds);
                const SpotShadowView spotView =
                    ComputeSpotShadowView(candidate.WorldPos, aimDir, light.Range, cone, fitBounds);
                // A spot/area uses face 0 only. Record carries the tile-remapped matrix
                // for the lighting pass; the raw array carries the un-remapped one
                // for the depth pass and per-view frustum cull.
                record.ViewProj[0] = ComposePunctualTileRemap(spotView.ViewProj, slot, 0);
                result.PunctualRawViewProj[slot][0] = spotView.ViewProj;
                // World per texel at the far plane: the fitted tile spans 2·far·tan(fovy/2)
                // across the tile's texels, so a narrow fitted cone yields a small bias — the
                // fit's tighter depth precision needs far less than the full range implied.
                const f32 worldPerTexel =
                    2.0f * spotView.Far * std::tan(spotView.Fovy * 0.5f) * invResolution;
                record.Params = vec4(2.0f, spotView.Near, spotView.Far, texelBias(worldPerTexel));
            }
            else
            {
                const PointShadowView pointView =
                    ComputePointShadowView(candidate.WorldPos, light.Range);
                for (u32 f = 0; f < CubeFaceCount; ++f)
                {
                    record.ViewProj[f] = ComposePunctualTileRemap(pointView.ViewProj[f], slot, f);
                    result.PunctualRawViewProj[slot][f] = pointView.ViewProj[f];
                }
                const f32 worldPerTexel = light.Range * 2.0f * invResolution;
                record.Params = vec4(1.0f, pointView.Near, pointView.Far, texelBias(worldPerTexel));
            }
            record.PositionRange = vec4(candidate.WorldPos, light.Range);

            candidate.PunctualSlot = static_cast<i32>(slot);
            ++result.PunctualCount;
        }

        // Lay the gathered lights out in scene iteration order; the ranking above decided only
        // which arm shadows which light, never where a light sits in the buffer.
        result.LightCount = candidateCount;
        for (u32 i = 0; i < candidateCount; ++i)
        {
            const LightCandidate& candidate = candidates[i];
            const Light& light = *candidate.Source;
            const mat4& world4 = candidate.World;
            const vec3& worldPos = candidate.WorldPos;
            const bool isArea = candidate.IsArea;
            const f32 shadowSlot = static_cast<f32>(candidate.PunctualSlot);
            // Stored as cosines for direct dot-product comparison in the shader.
            const f32 cosInner = std::cos(light.InnerCone);
            const f32 cosOuter = std::cos(light.OuterCone);

            // Area-light shape, packed into the last two vec4. A Rect or Polygon emits its
            // world-space vertices into the shared area-vertex buffer (base/count in Area.yz); a
            // Sphere records its transform-scaled radius in Area.x. Non-area lights leave these
            // inert (radius 0, count 0, area-shadow slot -1). The area-shadow slot stays -1 here;
            // the shading path is independent of the shadow arm.
            vec4 area{0.0f, 0.0f, 0.0f, -1.0f};
            vec3 areaNormal{0.0f};
            // The light's world-space size, driving the PCSS penumbra width in the lighting pass.
            f32 shadowRadius = 0.0f;
            const f32 flags = static_cast<f32>(PackLightFlags(light, candidate));

            if (light.Type == LightType::Sphere)
            {
                // Uniform-scale the authored radius by the transform's basis length.
                const f32 scale = glm::length(vec3(world4[0]));
                area.x = light.Radius * scale;
                shadowRadius = area.x;
            }
            else if (light.Type == LightType::Rect || light.Type == LightType::Polygon)
            {
                // Gather the light's local-space vertices: a Rect is four corners of its
                // Width × Height plane wound CCW about local +Z; a Polygon is its authored list.
                std::array<vec3, 4> rectLocal{};
                std::span<const vec3> localVerts;
                if (light.Type == LightType::Rect)
                {
                    const f32 hw = light.Width * 0.5f;
                    const f32 hh = light.Height * 0.5f;
                    rectLocal = {vec3(-hw, -hh, 0.0f), vec3(hw, -hh, 0.0f), vec3(hw, hh, 0.0f),
                                 vec3(-hw, hh, 0.0f)};
                    localVerts = rectLocal;
                }
                else
                {
                    localVerts = light.PolygonVertices;
                }

                // Emit world-space vertices, honoring the per-view cap: drop the light's area
                // geometry (count 0) rather than partially write it if it would overflow.
                const u32 count = static_cast<u32>(localVerts.size());
                if (count >= 3 &&
                    result.AreaVertexCount + count <= BindlessRegistry::MaxAreaVertices)
                {
                    const u32 base = result.AreaVertexCount;
                    for (u32 v = 0; v < count; ++v)
                    {
                        const vec3 worldVert = vec3(world4 * vec4(localVerts[v], 1.0f));
                        result.AreaVertices[base + v] = vec4(worldVert, 0.0f);
                    }
                    result.AreaVertexCount += count;
                    area.y = static_cast<f32>(base);
                    area.z = static_cast<f32>(count);

                    // Area normal from the first triangle's winding (CCW front face).
                    const vec3 v0 = vec3(result.AreaVertices[base + 0]);
                    const vec3 v1 = vec3(result.AreaVertices[base + 1]);
                    const vec3 v2 = vec3(result.AreaVertices[base + 2]);
                    const vec3 n = glm::cross(v1 - v0, v2 - v0);
                    const f32 nLen = glm::length(n);
                    areaNormal = nLen > 1e-6f ? n / nLen : vec3(0.0f, 0.0f, 1.0f);

                    // The light's bounding radius (farthest vertex from center) sizes the penumbra.
                    for (u32 v = 0; v < count; ++v)
                    {
                        shadowRadius =
                            std::max(shadowRadius,
                                     glm::length(vec3(result.AreaVertices[base + v]) - worldPos));
                    }
                }
            }

            result.Lights[i] = PackedLight{
                .PositionRange = vec4(worldPos, light.Range),
                .DirectionType = vec4(light.Direction, static_cast<f32>(light.Type)),
                .ColorIntensity = vec4(light.Color, light.Intensity),
                .Cone = vec4(cosInner, cosOuter, shadowSlot, flags),
                .Area = area,
                .AreaNormal = vec4(areaNormal, shadowRadius),
            };
        }

        return result;
    }
}
