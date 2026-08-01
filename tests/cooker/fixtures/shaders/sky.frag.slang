// Sky fragment fixture: writes a single float4 SV_Target0 (the Sky domain's radiance
// output contract) and reads a byte-address storage buffer through the set-0 g_Buffers[]
// bindless array selected by a StorageBufferHandle param. Used by the material-cook test
// to assert a Sky material cooks (domain 2, one target, a storagebuffer field) and that a
// Sky material against a g-buffer MRT shader fails the radiance contract.

struct MaterialParams
{
    float4 Tint;   // authored param
    uint   Points; // storage-buffer handle (runtime-bound)
    uint   Count;  // authored count
};

static const uint MaterialParamStride = 512;

[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct PushConstants
{
    uint MaterialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 accum = p.Tint;
    if (p.Count > 0)
    {
        accum += g_Buffers[p.Points].Load<float4>(0);
    }
    return accum;
}
