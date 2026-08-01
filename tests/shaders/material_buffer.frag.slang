// Reads a byte-address storage buffer whose slot is carried in a material's parameter
// block: the material-input two-hop the storage-buffer material feature performs. The
// pushed MaterialIndex selects the param block (set 0, binding 4); its first uint is the
// g_Buffers[] handle (what MaterialInstance::SetStorageBufferHandle writes); the shader
// loads a float4 from that buffer (set 0, binding 3) and outputs it. Proves a material
// reaching a game-supplied buffer by handle produces the buffer's contents.
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

static const uint MaterialParamStride = 512;

struct MaterialParams
{
    uint Points; // storage-buffer handle slot (SetStorageBufferHandle writes it)
};

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
float4 fsMain(VSOutput input) : SV_Target
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    return g_Buffers[p.Points].Load<float4>(0);
}
