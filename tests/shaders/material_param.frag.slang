// Reads a material's parameter block from the ring-buffered per-material storage
// buffer (set 0, binding 4) and outputs a float4 param as color. The pushed
// MaterialIndex already folds in the current frame-in-flight's region base
// (BindlessRegistry::GetCurrentFrameBase()), so the load at index * stride reads
// this frame's copy of the block. The param sits at byte offset 0 of the block
// (the test lays out a single float4 there).
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

static const uint MaterialParamStride = 256;

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
    return g_MaterialParams.Load<float4>(g_PC.MaterialIndex * MaterialParamStride);
}
