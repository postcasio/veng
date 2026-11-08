// Model portrait coverage fragment stage: copies a portrait's tonemapped colour and takes its alpha
// from the coverage the same render's HDR target accumulated, so the portrait composites,
// premultiplied, over whatever a UI draws behind it. The tonemap leaves alpha opaque; the HDR target
// carries the opaque coverage the lighting pass writes (1 on geometry, 0 on the background),
// accumulated with every translucent draw's coverage by `over`, an additive draw leaving it as it
// was. The colour is the subject tonemapped over black, which is already premultiplied by that
// coverage. Both inputs share the target's extent, so they are loaded, not sampled.
#include "Veng/bindless.slang"

struct PushConstants
{
    uint Color;    // the portrait renderer's tonemapped output (bindless index)
    uint Coverage; // the same render's HDR target, coverage in alpha (bindless index)
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    const int3 texel = int3(int2(input.sv_position.xy), 0);
    const float3 color = g_Textures[NonUniformIndex(g_PC.Color)].Load(texel).rgb;
    const float coverage = g_Textures[NonUniformIndex(g_PC.Coverage)].Load(texel).a;
    return float4(color, saturate(coverage));
}
