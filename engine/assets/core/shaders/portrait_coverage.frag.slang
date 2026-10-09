// Model portrait coverage fragment stage: copies a portrait's tonemapped colour and takes its alpha
// from the depth the same render wrote, so the portrait composites over whatever a UI draws behind
// it. The tonemap leaves alpha opaque, and a model drawn with no sky over an empty scene has nothing
// but depth to say where it is. Both inputs share the target's extent, so they are loaded, not
// sampled. Reverse-Z clears depth to 0: any greater value is geometry.
[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint Color; // the portrait renderer's tonemapped output (bindless index)
    uint Depth; // the same render's depth (bindless index)
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
    const int3 texel = int3(int2(input.sv_position.xy), 0);
    const float3 color = g_Textures[NonUniformResourceIndex(g_PC.Color)].Load(texel).rgb;
    const float depth = g_Textures[NonUniformResourceIndex(g_PC.Depth)].Load(texel).r;
    return float4(color, depth > 0.0 ? 1.0 : 0.0);
}
