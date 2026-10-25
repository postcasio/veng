// Translucent fragment fixture: writes a single float4 SV_Target0 (the Translucent domain's
// HDR-color-plus-alpha output contract). Self-contained like the sky fixture (it declares its own
// MaterialParams + selector), so the material-cook test exercises the domain validation without
// threading the engine shader include dir. Used to assert a Translucent material cooks (domain 3,
// one float4 target) and that a Translucent material against a g-buffer MRT shader fails the
// single-target contract; fsMainReactive adds the reactive amount a "reactive" material declares.

struct MaterialParams
{
    float4 Color; // HDR color + coverage in a
};

[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

// A Translucent material reads its selector from the v_MaterialOffset interpolant (the surface
// vertex path), so the fixture carries it on its input struct rather than a push block.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation uint v_MaterialOffset : TEXCOORD1;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    return p.Color;
}

// The reactive variant: the same colour, and its coverage as the reactive amount at SV_Target2, its
// outputs pinned to their locations. ImplicitReactiveOutput leaves them to Slang's declaration-order
// numbering, which puts the reactive amount at location 1, and the cook refuses it.
struct ReactiveOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(2)]] float Reactive : SV_Target2;
};

struct ImplicitReactiveOutput
{
    float4 Color : SV_Target0;
    float Reactive : SV_Target2;
};

[shader("fragment")]
ReactiveOutput fsMainReactive(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    ReactiveOutput output;
    output.Color = p.Color;
    output.Reactive = p.Color.a;
    return output;
}

[shader("fragment")]
ImplicitReactiveOutput fsMainReactiveImplicit(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    ImplicitReactiveOutput output;
    output.Color = p.Color;
    output.Reactive = p.Color.a;
    return output;
}

struct MeteringOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(3)]] float4 MeteringFlags : SV_Target3;
};

[shader("fragment")]
MeteringOutput fsMainMetering(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    MeteringOutput output;
    output.Color = p.Color;
    output.MeteringFlags = float4(0.0, 0.0, 0.0, 4.0 / 7.0);
    return output;
}
