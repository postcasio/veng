// Translucent fragment fixture: writes a single float4 SV_Target0 (the Translucent domain's
// HDR-color-plus-alpha output contract). Self-contained like the sky fixture (it declares its own
// MaterialParams + selector), so the material-cook test exercises the domain validation without
// threading the engine shader include dir. Used to assert a Translucent material cooks (domain 3,
// one float4 target) and that a Translucent material against a g-buffer MRT shader fails the
// single-target contract.

struct MaterialParams
{
    float4 Color; // HDR color + coverage in a
};

static const uint MaterialParamStride = 256;

[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

// A Translucent material reads its selector from the v_MaterialIndex interpolant (the surface
// vertex path), so the fixture carries it on its input struct rather than a push block.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation uint v_MaterialIndex : TEXCOORD1;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    return p.Color;
}
