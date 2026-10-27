// The flipbook shading model, with the half that reads the scene: the light a six-way flipbook
// receives from the view's lights and its ambient, gathered once per sprite (or per particle)
// rather than per pixel. The per-pixel arithmetic that turns texels and a gather into colour — six-way
// weighting, the temperature decode and emission ramp, the frame blend, the soft fade and the unlit
// path — is Veng/sprite_shading_core.slang, which this re-exports; a consumer with no scene to read
// includes that alone.
//
// The includer declares the view block (view_constants.slang) and the set-0 bindless arrays
// g_Textures / g_Samplers first. This file brings in, through Veng/medium_lighting.slang, the
// set-0 light buffer, the shadow system at set 5 and the IBL maps at set 4, and through
// Veng/scene_depth.slang the opaque-depth readers. A pass including it binds the renderer's IBL and
// shadow sets at 4 and 5, as the forward-lit translucent pass does.
//
// Nothing here takes a screen-space derivative or reads a stage built-in, so the gather runs in a
// vertex, fragment or compute stage alike; the shadow lookups sample at an explicit level of 0.

#pragma once

#include "Veng/medium_lighting.slang"
#include "Veng/scene_depth.slang"
#include "Veng/sh.slang"
#include "Veng/sprite_shading_core.slang"

// An area light as a small source of the same power, seen from a sprite: the factor its radiance
// (the emitter's luminance) takes to the irradiance it delivers at worldPos, where Point and Spot
// take PunctualLightFalloff. A Sphere is its luminance over its disc, L pi r^2 / d^2, exact outside
// it; a Rect or Polygon is its luminance over its area, foreshortened by the cosine it faces worldPos
// at (either face of a two-sided one), and nothing behind a one-sided one. Distances are floored at
// the larger of distanceFloor and the source's own size, and fade to the light's Range as a point
// light's do, measured from a Sphere's surface. A medium has no normal, so no receiver cosine is
// taken. L receives the unit direction toward the light's centre and dist the distance to it.
float SpriteAreaLightFalloff(ViewConstants vc, GpuLight light, float3 worldPos,
                             float distanceFloor, out float3 L, out float dist)
{
    float3 toLight = light.PositionRange.xyz - worldPos;
    dist = length(toLight);
    L = dist > 1e-4 ? toLight / dist : float3(0.0, 0.0, 1.0);
    float range = max(light.PositionRange.w, 1e-4);

    float type = light.DirectionType.w;
    float projected;
    float sourceSize;
    float reach = dist;
    if (type == LightTypeSphere)
    {
        float radius = light.Area.x;
        projected = SpriteShadingPi * radius * radius;
        sourceSize = radius;
        reach = max(dist - radius, 0.0);
    }
    else
    {
        uint base = uint(light.Area.y);
        uint count = uint(light.Area.z);
        if (count < 3u)
        {
            return 0.0;
        }
        float3 first = LoadAreaVertex(vc, base);
        float3 twiceArea = float3(0.0);
        for (uint i = 1u; i + 1u < count; ++i)
        {
            twiceArea += cross(LoadAreaVertex(vc, base + i) - first,
                               LoadAreaVertex(vc, base + i + 1u) - first);
        }
        float area = 0.5 * length(twiceArea);
        float facing = dot(light.AreaNormal.xyz, -L);
        facing = LightIsTwoSided(light.Cone.w) ? abs(facing) : max(facing, 0.0);
        projected = area * facing;
        sourceSize = sqrt(area / SpriteShadingPi);
    }
    if (projected <= 0.0 || reach >= range)
    {
        return 0.0;
    }
    float floorRadius = max(sourceSize, distanceFloor);
    float rangeFactor = saturate(1.0 - pow(reach / range, 4.0));
    rangeFactor *= rangeFactor;
    return projected / max(max(dist * dist, floorRadius * floorRadius), 1e-4) * rangeFactor;
}

// The radiance arriving along each of a six-way flipbook's axes from this view's lights, shadowed. centre is the sprite's world centre, X, Y and Z its frame's unit
// world axes (X right, Y up, Z toward the viewer), and radius its world radius. The result's
// Ambient is zero; SpriteAmbient supplies it.
//
// - A directional light arrives along its own direction, at its full radiance.
// - A point or spot light is attenuated by PunctualLightFalloff with a distance floor of radius, so
//   a light inside the sprite reaches it as if from its surface rather than blowing up, and its
//   weights blend from isotropic to directional by saturate(dist / radius), so a light at the
//   centre lights every face alike.
// - An area light (Rect, Sphere, Polygon) arrives as a small source of the same power from its
//   centre (SpriteAreaLightFalloff), with the same distance floor and directional blend.
//
// Visibility is LightVisibility at the centre through the 3×3 PCF kernel, one lookup per light per
// sprite, so a sprite drifting across a shadow's edge fades rather than popping.
SpriteLightGather EvaluateSpriteLights(ViewConstants vc, float3 centre, float3 X, float3 Y,
                                       float3 Z, float radius)
{
    SpriteLightGather gather = SpriteLightGatherNone();
    uint count = vc.LightState.y;
    for (uint i = 0; i < count; ++i)
    {
        GpuLight light = LoadLight(vc, i);
        float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a;
        float type = light.DirectionType.w;
        if (all(radiance == float3(0.0)))
        {
            continue;
        }

        float3 L;
        float attenuation = 1.0;
        float directionality = 1.0;
        if (type == LightTypeDirectional)
        {
            L = normalize(-light.DirectionType.xyz);
        }
        else
        {
            float dist;
            if (type > LightTypeSpot + 0.5)
            {
                attenuation = SpriteAreaLightFalloff(vc, light, centre, radius, L, dist);
            }
            else
            {
                float sinSource;
                attenuation = PunctualLightFalloff(light, centre, radius, L, dist, sinSource);
            }
            if (attenuation <= 0.0)
            {
                continue;
            }
            directionality = saturate(dist / max(radius, 1e-4));
        }

        float visibility = LightVisibility(vc, i, centre, LightVisibilityPcf);
        float3 d = float3(dot(L, X), dot(L, Y), dot(L, Z));
        AddSixWayLight(gather, d, radiance * (attenuation * visibility), directionality);
    }
    return gather;
}

// The ambient term of the view's ambient arm for a white diffuse surface facing direction n, with
// no occlusion: the flat floor, the SH skylight, or the image-based irradiance — the diffuse arms
// of the surface lighting core's ambient, with no Fresnel split, since a medium has no specular
// lobe.
float3 SpriteAmbientToward(ViewConstants vc, float3 n)
{
    uint arm = vc.LightState.w;
    if (arm == AmbientArmIbl)
    {
        return g_Irradiance.SampleLevel(g_IblSampler, EnvironmentDirection(vc, n), 0.0).rgb *
               vc.AmbientFloor.w * vc.EnvironmentLightTint.rgb;
    }
    if (arm == AmbientArmSkylight)
    {
        return max(ShEvalIrradiance(vc.SkyShCoeffs, SkyDirection(vc, n)), float3(0.0)) /
                   SpriteShadingPi *
               vc.AmbientParams.x * vc.EnvironmentLightTint.rgb;
    }
    return vc.AmbientFloor.rgb;
}

// The ambient term of a six-way flipbook: SpriteAmbientToward averaged over its frame's six
// directions ±X, ±Y, ±Z.
float3 SpriteAmbient(ViewConstants vc, float3 X, float3 Y, float3 Z)
{
    float3 sum = SpriteAmbientToward(vc, X) + SpriteAmbientToward(vc, -X) +
                 SpriteAmbientToward(vc, Y) + SpriteAmbientToward(vc, -Y) +
                 SpriteAmbientToward(vc, Z) + SpriteAmbientToward(vc, -Z);
    return sum / 6.0;
}

// Everything a six-way flipbook receives from the scene: EvaluateSpriteLights with SpriteAmbient as
// its Ambient. The one call a sprite or a particle makes per frame; its result feeds ShadeSixWay.
SpriteLightGather EvaluateSpriteLighting(ViewConstants vc, float3 centre, float3 X, float3 Y,
                                         float3 Z, float radius)
{
    SpriteLightGather gather = EvaluateSpriteLights(vc, centre, X, Y, Z, radius);
    gather.Ambient = SpriteAmbient(vc, X, Y, Z);
    return gather;
}
