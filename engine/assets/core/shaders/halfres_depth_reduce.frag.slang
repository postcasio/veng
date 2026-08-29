// Half-resolution depth reduce: writes each half-res texel's depth as the FARTHEST of the
// 2x2 full-resolution opaque depths it covers. Reverse-Z is in use (near = 1, far = 0), so
// farthest is the minimum. The reduced target is what the half-resolution translucent layer
// depth-tests against; taking the farthest sample is conservative — a half-res fragment
// survives wherever any of its four full-res pixels would show it, and the depth-aware
// composite upsample resolves the edges the conservative test lets through.
[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint DepthTexture;
    uint ValidX;
    uint ValidY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct FSOutput
{
    float sv_depth : SV_Depth;
};

[shader("fragment")]
FSOutput fsMain(VSOutput input)
{
    Texture2D depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];
    // The viewport covers the half-res valid sub-rect, so sv_position is in half pixels; the
    // 2x2 block it covers starts at twice that, clamped inside the full valid sub-rect (an odd
    // valid extent leaves the last row/column a partial block).
    const int2 base = int2(input.sv_position.xy) * 2;
    const int2 maxCoord = int2(int(g_PC.ValidX) - 1, int(g_PC.ValidY) - 1);
    float farthest = 1.0;
    [unroll]
    for (int dy = 0; dy < 2; ++dy)
    {
        [unroll]
        for (int dx = 0; dx < 2; ++dx)
        {
            const int2 coord = min(base + int2(dx, dy), maxCoord);
            farthest = min(farthest, depth.Load(int3(coord, 0)).r);
        }
    }
    FSOutput output;
    output.sv_depth = farthest;
    return output;
}
