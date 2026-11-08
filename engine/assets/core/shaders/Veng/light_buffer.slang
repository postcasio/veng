// The per-view light buffers and the packed light record: what the renderer writes for each view's
// lights, and the accessors that read them back. Shared by the lighting core (Veng/lighting.slang)
// and by any pass that reasons about the view's lights without shading — the light-tile cull reads
// the same records to bound each light's reach — so the layout is declared once.
//
// The includer declares the view block (view_constants.slang) before including this. The set-0
// light and area-vertex buffers (g_Lights and g_AreaVertices, bindings 6 and 7) come from
// bindless.slang.
//
// PunctualLightFalloff is the one statement of how a point or spot light weakens with distance and
// angle, shared by the surface light loop and by anything else a punctual light reaches.

#pragma once

// The area-vertex buffer holds Rect/Polygon world-space vertices, one vec4 each. A light's Area.y/z
// give the base/count relative to this view's region; the view block's area-vertex base folds in
// the region start, exactly as its light base does for the light buffer.
#include "bindless.slang"

static const uint LightStride = 112;
static const uint AreaVertexStride = 16;

// The view block's ambient arm (LightState.w), matching the renderer's AmbientArm.
static const uint AmbientArmFlat = 0;
static const uint AmbientArmSkylight = 1;
static const uint AmbientArmIbl = 2;

// LightType, matching the Light builtin's enum.
static const float LightTypeDirectional = 0.0;
static const float LightTypePoint = 1.0;
static const float LightTypeSpot = 2.0;
static const float LightTypeRect = 3.0;
static const float LightTypeSphere = 4.0;
static const float LightTypePolygon = 5.0;

// One light, packed by the renderer into the ring-buffered light buffer. Position
// is the entity's world transform; the type rides DirectionType.w. Area and AreaNormal
// carry the source/shape size: a Sphere's or a Point/Spot's source radius, a Directional's
// sine of angular radius (dimensionless, not a world radius), a Rect/Polygon's vertex range
// into the area-vertex buffer, the area-shadow slot, and the world area normal. Response
// weights the light's terms, and carries the light's authored tag (see LoadLightTag).
struct GpuLight
{
    float4 PositionRange;   // xyz world position, w range
    float4 DirectionType;   // xyz travel direction, w LightType
    float4 ColorIntensity;  // rgb linear color, a intensity
    float4 Cone;            // x cos(inner), y cos(outer), z punctual shadow slot, w LightFlag bits
    float4 Area;            // x sphere/point/spot source radius or directional sin(angular radius), y poly vertex base, z poly vertex count, w area-shadow slot
    float4 AreaNormal;      // xyz world area normal (Rect/Polygon), w shadow source radius
    float4 Response;        // x specular scale, y the tag's u32 bits (LoadLightTag), zw unused
};

// The index-th light of this view's region.
GpuLight LoadLight(ViewConstants vc, uint index)
{
    return g_Lights.Load<GpuLight>((vc.LightState.x + index) * LightStride);
}

// Byte offset of the tag lane, Response.y, within a packed light.
static const uint LightTagOffset = 100;

// The tag the scene authored on the index-th light of this view's region (Light::Tag), 0 when
// untagged. The lane holds the tag's u32 bit pattern rather than a float value, so it is loaded as
// raw bits: a tag past 2^24, or one whose bits spell a NaN, survives exactly.
uint LoadLightTag(ViewConstants vc, uint index)
{
    return g_Lights.Load<uint>((vc.LightState.x + index) * LightStride + LightTagOffset);
}

// Load a packed area-light polygon vertex (world space) by its region-relative index.
float3 LoadAreaVertex(ViewConstants vc, uint index)
{
    return g_AreaVertices.Load<float4>((vc.LightState.z + index) * AreaVertexStride).xyz;
}

// The most lights a view holds. Mirrors BindlessRegistry::MaxLights.
static const uint MaxLights = 128;

// The 32-bit words a LightMask spans, one bit per light a view can hold.
static const uint LightMaskWords = MaxLights / 32u;

// A subset of a view's lights: bit (i % 32) of word (i / 32) names the view's i-th light.
struct LightMask
{
    uint Words[LightMaskWords];
};

// A light mask naming every light of a view holding count lights.
LightMask AllLightsMask(uint count)
{
    LightMask mask;
    for (uint i = 0u; i < LightMaskWords; ++i)
    {
        uint first = i * 32u;
        mask.Words[i] = count >= first + 32u ? 0xFFFFFFFFu
                                             : (count > first ? (1u << (count - first)) - 1u : 0u);
    }
    return mask;
}

// Byte offset of the type lane, DirectionType.w, within a packed light.
static const uint LightTypeOffset = 28;

// A light mask naming every directional light of this view: the lights with no position, which
// reach every point alike. Reads only each light's type lane.
LightMask DirectionalLightsMask(ViewConstants vc)
{
    LightMask mask;
    for (uint i = 0u; i < LightMaskWords; ++i)
    {
        mask.Words[i] = 0u;
    }
    uint count = min(vc.LightState.y, MaxLights);
    for (uint i = 0u; i < count; ++i)
    {
        float type = asfloat(g_Lights.Load((vc.LightState.x + i) * LightStride + LightTypeOffset));
        if (type == LightTypeDirectional)
        {
            mask.Words[i / 32u] |= 1u << (i % 32u);
        }
    }
    return mask;
}

// The attenuation of a point or spot light at worldPos: the inverse-square falloff, its smooth
// cutoff at the light's Range, and a spot's cone. Returns 0 at or past the range and outside the
// cone. L receives the unit direction from worldPos toward the light ((0, 0, 1) when the two
// coincide), dist the distance to it, and sinSource the sine of the angular radius the light's
// source sphere subtends there.
//
// The inverse square is taken over a distance floored at the larger of the light's source radius
// (Area.x) and distanceFloor, so a real source size — not an epsilon guard — caps the response at
// contact; with both zero the 1e-4 floor makes it a pure inverse square. A 1/(d^2 + 1)
// denominator is deliberately not used: the +1 perturbs the falloff at every distance, not only
// near the singularity, so it cannot be read as absolute radiance under a fixed exposure. L still
// comes from the true offset — the floor bounds the magnitude of the response, not its direction.
// A receiver with an extent of its own passes it as distanceFloor, so a light inside it reaches it
// as if from its surface.
float PunctualLightFalloff(GpuLight light, float3 worldPos, float distanceFloor, out float3 L,
                           out float dist, out float sinSource)
{
    float3 toLight = light.PositionRange.xyz - worldPos;
    dist = length(toLight);
    float range = max(light.PositionRange.w, 1e-4);
    L = dist > 1e-4 ? toLight / dist : float3(0.0, 0.0, 1.0);
    float sourceRadius = light.Area.x;
    // The same radius sizes a lobe by the angle it subtends here (a hemisphere inside it).
    sinSource = min(sourceRadius / max(dist, 1e-4), 1.0);
    if (dist >= range)
    {
        return 0.0;
    }

    float floorRadius = max(sourceRadius, distanceFloor);
    float distanceFactor = 1.0 / max(max(dist * dist, floorRadius * floorRadius), 1e-4);
    float rangeFactor = saturate(1.0 - pow(dist / range, 4.0));
    rangeFactor *= rangeFactor;
    float attenuation = distanceFactor * rangeFactor;

    if (light.DirectionType.w == LightTypeSpot)
    {
        // Smoothstep the cone between cos(inner) (full) and cos(outer) (zero); the
        // receiver-to-light direction is compared against the light's travel direction.
        float cosAngle = dot(normalize(-light.DirectionType.xyz), L);
        attenuation *= smoothstep(light.Cone.y, light.Cone.x, cosAngle);
    }
    return attenuation;
}
