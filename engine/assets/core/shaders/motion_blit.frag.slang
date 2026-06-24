// Motion-vector visualize fragment stage: samples the per-object velocity g-buffer
// channel (G3, the surface pass's RG16Sfloat screen-space motion, curUV - prevUV) through
// the bindless set-0 array and colorizes it as an optical-flow field — hue encodes
// the motion direction (a full color wheel), brightness its magnitude. A static
// surface under a static camera reads zero and renders black; the cleared background
// (no geometry wrote it) is likewise black. A debug view of the input the TAA resolve
// reprojects history through.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint VelocityTexture;
    uint Sampler;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

static float3 HueToRgb(float hue)
{
    // hue in [0, 1] → fully-saturated RGB around the color wheel.
    const float r = abs(hue * 6.0 - 3.0) - 1.0;
    const float g = 2.0 - abs(hue * 6.0 - 2.0);
    const float b = 2.0 - abs(hue * 6.0 - 4.0);
    return saturate(float3(r, g, b));
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    const float2 velocity =
        g_Textures[NonUniformResourceIndex(g_PC.VelocityTexture)].Sample(
            g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).rg;

    // Velocity is a UV displacement, so its magnitude is tiny (a fast pan is a few
    // percent of the screen). A perceptual gain spreads sub-pixel-to-large motion
    // across the brightness range without clipping the common case to white.
    const float magnitude = length(velocity);
    const float brightness = saturate(sqrt(magnitude * 8.0));

    // atan2 over the full circle → hue, so opposite directions read as opposite colors.
    const float PI = 3.14159265;
    const float hue = atan2(velocity.y, velocity.x) / (2.0 * PI) + 0.5;

    return float4(HueToRgb(hue) * brightness, 1.0);
}
