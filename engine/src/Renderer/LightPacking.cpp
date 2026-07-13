#include <Veng/Renderer/LightPacking.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>
#include <cmath>
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
                                      const u32 punctualShadowResolution)
    {
        PackedSceneLights result;

        for (auto [entity, light] : world.View<Light>())
        {
            if (result.LightCount >= SceneView::MaxLights)
            {
                break;
            }

            // Shadow the first directional light; its direction drives the light-space matrix.
            if (!result.HaveDirectional && light.Type == LightType::Directional)
            {
                result.HaveDirectional = true;
                result.DirectionalTravel = light.Direction;
            }

            const mat4 world4 = WorldMatrix(world, entity);
            const vec3 worldPos = vec3(world4[3]);
            // Stored as cosines for direct dot-product comparison in the shader.
            const f32 cosInner = std::cos(light.InnerCone);
            const f32 cosOuter = std::cos(light.OuterCone);

            const bool isArea = light.Type == LightType::Rect || light.Type == LightType::Sphere ||
                                light.Type == LightType::Polygon;

            // Assign a shadow slot to the first MaxShadowedPunctual point/spot/area lights;
            // the rest carry -1. With punctual shadows off all lights carry -1. Point uses six
            // cube faces; spot and area use a single perspective map (area aimed along Direction,
            // softened per-light-size by PCSS in the lighting pass).
            f32 shadowSlot = -1.0f;
            if (punctualShadows && result.PunctualCount < MaxShadowedPunctual &&
                (light.Type == LightType::Point || light.Type == LightType::Spot || isArea))
            {
                const u32 slot = result.PunctualCount;
                PunctualShadowRecord& record = result.PunctualRecords[slot];
                // Depth bias scales with world units per texel: a coarser tile (larger
                // range or smaller resolution) needs more bias. The shader adds a
                // slope-scaled term on top.
                const f32 worldPerTexel =
                    light.Range * 2.0f / static_cast<f32>(punctualShadowResolution);
                const f32 punctualBias = std::clamp(worldPerTexel * 0.5f, 0.0005f, 0.01f);
                if (light.Type != LightType::Point)
                {
                    // Aim the perspective map along the light's travel direction; area lights use a
                    // wide fixed cone, a spot its own outer cone.
                    const f32 dirLen = glm::length(light.Direction);
                    const vec3 aimDir =
                        dirLen > 1e-5f ? light.Direction / dirLen : vec3(0.0f, -1.0f, 0.0f);
                    const f32 cone = isArea ? AreaShadowCone : light.OuterCone;
                    const SpotShadowView spotView =
                        ComputeSpotShadowView(worldPos, aimDir, light.Range, cone);
                    // A spot/area uses face 0 only. Record carries the tile-remapped matrix
                    // for the lighting pass; the raw array carries the un-remapped one
                    // for the depth pass and per-view frustum cull.
                    record.ViewProj[0] = ComposePunctualTileRemap(spotView.ViewProj, slot, 0);
                    result.PunctualRawViewProj[slot][0] = spotView.ViewProj;
                    record.Params = vec4(2.0f, spotView.Near, spotView.Far, punctualBias);
                }
                else
                {
                    const PointShadowView pointView = ComputePointShadowView(worldPos, light.Range);
                    for (u32 f = 0; f < CubeFaceCount; ++f)
                    {
                        record.ViewProj[f] =
                            ComposePunctualTileRemap(pointView.ViewProj[f], slot, f);
                        result.PunctualRawViewProj[slot][f] = pointView.ViewProj[f];
                    }
                    record.Params = vec4(1.0f, pointView.Near, pointView.Far, punctualBias);
                }
                record.PositionRange = vec4(worldPos, light.Range);

                shadowSlot = static_cast<f32>(slot);
                ++result.PunctualCount;
            }

            // Area-light shape, packed into the last two vec4. A Rect or Polygon emits its
            // world-space vertices into the shared area-vertex buffer (base/count in Area.yz); a
            // Sphere records its transform-scaled radius in Area.x. Non-area lights leave these
            // inert (radius 0, count 0, area-shadow slot -1). The area-shadow slot stays -1 here;
            // the shading path is independent of the shadow arm.
            vec4 area{0.0f, 0.0f, 0.0f, -1.0f};
            vec3 areaNormal{0.0f};
            // The light's world-space size, driving the PCSS penumbra width in the lighting pass.
            f32 shadowRadius = 0.0f;
            const f32 flags = light.TwoSided ? 1.0f : 0.0f;

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

            result.Lights[result.LightCount] = PackedLight{
                .PositionRange = vec4(worldPos, light.Range),
                .DirectionType = vec4(light.Direction, static_cast<f32>(light.Type)),
                .ColorIntensity = vec4(light.Color, light.Intensity),
                .Cone = vec4(cosInner, cosOuter, shadowSlot, flags),
                .Area = area,
                .AreaNormal = vec4(areaNormal, shadowRadius),
            };
            ++result.LightCount;
        }

        return result;
    }
}
