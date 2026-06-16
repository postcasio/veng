// Deferred lighting fragment stage: samples the g-buffer (G0 albedo, G1
// world-space normal, the depth attachment) through the bindless set-0 array,
// applies a single directional light, and writes the lit color into the HDR
// target. A minimal Lambert model — N·L × color × intensity — plus a small
// constant ambient term so unlit/back faces and the no-light default are never
// pure black.
//
// Color space: G0 is an sRGB target, so the sample decodes albedo to linear; the
// HDR output is a linear float target, so the lit color stays linear (the tail
// tonemap/composite handles display encoding). World-space normals are sampled
// straight from G1 (a signed float target), so no reconstruction from depth is
// needed for the N·L term.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint AlbedoTexture; // G0 bindless index
    uint NormalTexture; // G1 bindless index
    uint DepthTexture;  // depth bindless index (reserved for world-position math)
    uint Sampler;       // shared sampler index
    float4 LightDirection; // xyz: world-space travel direction; w unused
    float4 LightColor;     // rgb: linear color; a: intensity
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// A small constant ambient term, applied to the albedo so a back/shadowed face
// — and a scene with no light — renders flat-ambient, never pure black.
static const float Ambient = 0.03;

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];

    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Sample(samp, input.v_UV).rgb;
    float3 normal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, input.v_UV).xyz;

    float normalLength = length(normal);
    float3 N = normalLength > 1e-4 ? normal / normalLength : float3(0.0, 0.0, 0.0);

    // The light direction is the direction the light travels; the surface faces
    // the light along -direction, so N·L uses the negated, normalized direction.
    float3 L = normalize(-g_PC.LightDirection.xyz);
    float intensity = g_PC.LightColor.a;
    float nDotL = max(dot(N, L), 0.0);

    float3 diffuse = albedo * g_PC.LightColor.rgb * (nDotL * intensity);
    float3 ambient = albedo * Ambient;

    return float4(ambient + diffuse, 1.0);
}
