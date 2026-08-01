// Translucent fragment fixture: writes the domain's single float4 SV_Target0 and, beside it, a
// float SV_Depth. Self-contained like the other domain fixtures (it declares its own
// MaterialParams + selector), so the material-cook test exercises the domain validation without
// threading the engine shader include dir. Used to assert that a depth member rides alongside a
// domain's color targets rather than counting as one of them.

struct MaterialParams
{
    float4 Color; // HDR color + coverage in a
};

static const uint MaterialParamStride = 512;

[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation uint v_MaterialIndex : TEXCOORD1;
};

struct FSOutput
{
    float4 Color : SV_Target0;
    float Depth : SV_Depth;
};

[shader("fragment")]
FSOutput fsMain(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    FSOutput o;
    o.Color = p.Color;
    o.Depth = saturate(input.sv_position.z);
    return o;
}
