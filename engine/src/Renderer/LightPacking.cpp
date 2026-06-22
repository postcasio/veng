#include <Veng/Renderer/LightPacking.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>
#include <cmath>

namespace Veng::Renderer
{
    namespace
    {
        // The punctual shadow atlas tile grid: CubeFaceCount columns × MaxShadowedPunctual
        // rows. A shadowed light's slot s, face f maps to tile (column f, row s) — so a spot
        // uses tile (0, s) and a point uses the whole of row s.
        constexpr u32 PunctualAtlasColumns = CubeFaceCount;
        constexpr u32 PunctualAtlasRows = MaxShadowedPunctual;

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

            // Assign a shadow slot to the first MaxShadowedPunctual point/spot lights;
            // the rest carry -1. With punctual shadows off all lights carry -1.
            f32 shadowSlot = -1.0f;
            if (punctualShadows && result.PunctualCount < MaxShadowedPunctual &&
                (light.Type == LightType::Point || light.Type == LightType::Spot))
            {
                const u32 slot = result.PunctualCount;
                PunctualShadowRecord& record = result.PunctualRecords[slot];
                // Depth bias scales with world units per texel: a coarser tile (larger
                // range or smaller resolution) needs more bias. The shader adds a
                // slope-scaled term on top.
                const f32 worldPerTexel =
                    light.Range * 2.0f / static_cast<f32>(punctualShadowResolution);
                const f32 punctualBias = std::clamp(worldPerTexel * 0.5f, 0.0005f, 0.01f);
                if (light.Type == LightType::Spot)
                {
                    const SpotShadowView spotView = ComputeSpotShadowView(
                        worldPos, light.Direction, light.Range, light.OuterCone);
                    // A spot uses face 0 only. Record carries the tile-remapped matrix
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

            result.Lights[result.LightCount] = PackedLight{
                .PositionRange = vec4(worldPos, light.Range),
                .DirectionType = vec4(light.Direction, static_cast<f32>(light.Type)),
                .ColorIntensity = vec4(light.Color, light.Intensity),
                .Cone = vec4(cosInner, cosOuter, shadowSlot, 0.0f),
            };
            ++result.LightCount;
        }

        return result;
    }
}
