// A simple authored Sky-domain material: a vertical gradient sky with a handful of
// bright points read from a game-supplied storage buffer. It demonstrates the engine's
// Sky material domain (view-ray reconstruction + g-buffer depth-mask discard, radiance
// composited in the sky slot) and the storage-buffer material input (a material reading
// a bindless byte-address buffer by handle). The sky physics is trivial and generic —
// the point buffer stands in for any data-driven sky (a starfield, a palette, an
// instancing set); its contents are the consumer's, not the engine's.
#include "Veng/sky.slang"

// This material's parameter block: the horizon/zenith gradient colors and the
// point-buffer handle + count. Handle and count are runtime-bound/authored uints; the
// gradient colors are authored vec4s. Large-alignment (vec4) fields first so the cooker's
// std140 offsets and the shader's scalar Load agree.
struct MaterialParams
{
    float4 HorizonColor; // rgb sky color at the horizon
    float4 ZenithColor;  // rgb sky color at the zenith
    uint   Points;       // storage-buffer handle: the point array (runtime-bound)
    uint   PointCount;   // number of valid points in the buffer (authored)
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// One point in the game-supplied buffer: a world-space direction (unit) and a color.
// The shader owns this struct; the engine holds the buffer as opaque bytes.
struct SkyPoint
{
    float3 Direction;
    float  Size;   // angular size falloff control
    float3 Color;
    float  Pad;
};

[[vk::push_constant]] ConstantBuffer<SkyPushConstants> g_SkyPC;

[shader("fragment")]
float4 fsMain(SkyFragmentInput input) : SV_Target0
{
    // The sky is a background composite: leave any foreground (lit geometry) untouched.
    if (!SkyIsBackground(g_SkyPC, input.v_UV))
    {
        discard;
    }

    MaterialParams p = LoadMaterialParams(g_SkyPC.MaterialIndex);
    float3 dir = SkyViewDirection(g_SkyPC, input.v_UV);

    // Vertical gradient from horizon to zenith by the ray's up component.
    float t = saturate(dir.y * 0.5 + 0.5);
    float3 sky = lerp(p.HorizonColor.rgb, p.ZenithColor.rgb, t);

    // Add each game-supplied point as a soft bright dot along its direction. Reads the
    // point array typed from the bindless byte-address buffer the material's Points handle
    // names — the handle is uniform across the draw, so no NonUniformResourceIndex.
    for (uint i = 0; i < p.PointCount; ++i)
    {
        SkyPoint pt = g_Buffers[p.Points].Load<SkyPoint>(i * 32);
        float c = dot(dir, normalize(pt.Direction));
        float falloff = smoothstep(1.0 - pt.Size, 1.0, c);
        sky += pt.Color * falloff;
    }

    return float4(sky, 1.0);
}
