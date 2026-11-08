// The field a layered sky bake's later layers read. A bake whose first layer has the Field role
// (BakedSkyCube::SkyBakeLayer) evaluates that layer at a reduced face resolution into up to four
// field cubes of physical quantities rather than a picture; each later layer of the same bake reads
// them through these helpers at full resolution, so an expensive, smooth evaluation runs coarsely
// while the composite that turns it into radiance runs at every texel.
//
// A Sky material reading the field includes this in place of Veng/sky.slang, which it re-exports.
// Outside a field bake's later layers — a direct sky, a picture-only bake, the field layer itself —
// every helper returns zero.
#pragma once

#include "sky.slang"

// The cube set (g_Cubes, set 2) holds the field targets; bindless.slang declares it.
#include "bindless.slang"

// The field's target count in this view: zero outside a field bake's later layers.
uint SkyBakeFieldTargetCount(SkyPushConstants pc)
{
    return LoadViewConstants(pc.ViewConstantsIndex).SkyBakeFieldInfo.x;
}

// The cube face a direction falls in and its face coordinates in [-1, 1], in the Vulkan cube-map
// convention the bake's face bases share (face order +X, -X, +Y, -Y, +Z, -Z).
void SkyBakeCubeFace(float3 d, out uint face, out float2 st)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0.0 ? 0u : 1u;
        st = (d.x > 0.0 ? float2(-d.z, -d.y) : float2(d.z, -d.y)) / a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0.0 ? 2u : 3u;
        st = (d.y > 0.0 ? float2(d.x, d.z) : float2(d.x, -d.z)) / a.y;
    }
    else
    {
        face = d.z > 0.0 ? 4u : 5u;
        st = (d.z > 0.0 ? float2(d.x, -d.y) : float2(-d.x, -d.y)) / a.z;
    }
}

// The (unnormalised) direction through face coordinates st of a cube face, the inverse of
// SkyBakeCubeFace. Coordinates past [-1, 1] continue the face's plane onto its neighbours.
float3 SkyBakeCubeDirection(uint face, float2 st)
{
    switch (face)
    {
    case 0u:
        return float3(1.0, -st.y, -st.x);
    case 1u:
        return float3(-1.0, -st.y, st.x);
    case 2u:
        return float3(st.x, 1.0, st.y);
    case 3u:
        return float3(st.x, -1.0, -st.y);
    case 4u:
        return float3(st.x, -st.y, 1.0);
    default:
        return float3(-st.x, -st.y, -1.0);
    }
}

// Field target `target` along `dir`, filtered with a cubic B-spline over its texels: four bilinear
// taps whose positions and weights reproduce the 4x4 B-spline kernel, laid out on the face the
// direction falls in. A tap past the face's edge lands on its neighbour, so the filter crosses edges
// and corners. The B-spline is smooth to its second derivative, so an upsample of eight or more
// texels per field texel shows no grid; it smooths rather than interpolates, by about a third of a
// texel's width squared. Zero outside a field bake's later layers.
float4 SkyBakeField(SkyPushConstants pc, uint target, float3 dir)
{
    const ViewConstants vc = LoadViewConstants(pc.ViewConstantsIndex);
    if (target >= vc.SkyBakeFieldInfo.x)
    {
        return float4(0.0);
    }
    const TextureCube cube = g_Cubes[NonUniformIndex(vc.SkyBakeFieldCubes[target])];
    const SamplerState samp = g_Samplers[NonUniformIndex(vc.SkyBakeFieldInfo.z)];
    const float size = float(vc.SkyBakeFieldInfo.w);

    uint face;
    float2 st;
    SkyBakeCubeFace(dir, face, st);
    const float2 x = (st * 0.5 + 0.5) * size - 0.5;
    const float2 i = floor(x);
    const float2 f = x - i;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    const float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    const float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    const float2 w3 = f3 / 6.0;
    const float2 g0 = w0 + w1;
    const float2 g1 = w2 + w3;
    // Each tap sits between two texel centres (texel k's centre is k + 0.5) where bilinear weighs
    // them as the B-spline does.
    const float2 p0 = ((i - 0.5 + w1 / g0) / size) * 2.0 - 1.0;
    const float2 p1 = ((i + 1.5 + w3 / g1) / size) * 2.0 - 1.0;

    return g0.x * g0.y * cube.SampleLevel(samp, SkyBakeCubeDirection(face, float2(p0.x, p0.y)), 0.0) +
           g1.x * g0.y * cube.SampleLevel(samp, SkyBakeCubeDirection(face, float2(p1.x, p0.y)), 0.0) +
           g0.x * g1.y * cube.SampleLevel(samp, SkyBakeCubeDirection(face, float2(p0.x, p1.y)), 0.0) +
           g1.x * g1.y * cube.SampleLevel(samp, SkyBakeCubeDirection(face, float2(p1.x, p1.y)), 0.0);
}

// Field target `target` along `dir` at mip `lod`, bilinear within and between levels. Each level is
// a 2x2 box reduction of the one above it, so level n averages 2^n field texels on a side. Zero
// outside a field bake's later layers.
float4 SkyBakeFieldLod(SkyPushConstants pc, uint target, float3 dir, float lod)
{
    const ViewConstants vc = LoadViewConstants(pc.ViewConstantsIndex);
    if (target >= vc.SkyBakeFieldInfo.x)
    {
        return float4(0.0);
    }
    return g_Cubes[NonUniformIndex(vc.SkyBakeFieldCubes[target])].SampleLevel(
        g_Samplers[NonUniformIndex(vc.SkyBakeFieldInfo.z)], dir, lod);
}

// The mean of field target `target` over the sphere, each field texel weighted by the solid angle
// it covers. A cube texel at a face's centre covers about five times the solid angle of one at its
// corner, so the box-reduced mip chain is not this mean; the bake reduces each face with the texels'
// exact solid angles, and this sums the six faces. Zero outside a field bake's later layers.
float4 SkyBakeFieldMean(SkyPushConstants pc, uint target)
{
    const ViewConstants vc = LoadViewConstants(pc.ViewConstantsIndex);
    if (target >= vc.SkyBakeFieldInfo.x)
    {
        return float4(0.0);
    }
    const ByteAddressBuffer sums = g_Buffers[NonUniformIndex(vc.SkyBakeFieldInfo.y)];
    float4 total = float4(0.0);
    for (uint face = 0u; face < 6u; ++face)
    {
        total += sums.Load<float4>((target * 6u + face) * 16u);
    }
    // The texels' exact solid angles sum to the sphere's.
    return total / (4.0 * 3.14159265358979);
}
