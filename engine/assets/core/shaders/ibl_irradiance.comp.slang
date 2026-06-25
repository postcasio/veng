// Diffuse irradiance convolution: for each output texel's normal direction, integrate the
// cosine-weighted hemisphere of incoming radiance from the source radiance cubemap. The
// result is the diffuse IBL term the lighting pass multiplies by albedo.
//
// Source radiance cube and destination irradiance cube both bind on set 1 (a closed
// producer->consumer chain off the bindless registry).
[[vk::binding(0, 1)]] TextureCube g_Env;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2DArray<float4> g_Dest;

struct PushConstants
{
    uint FaceSize; // destination face edge length in texels
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const float Pi = 3.14159265358979323846;

float3 FaceDirection(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
    case 0: dir = float3(1.0, -st.y, -st.x); break;
    case 1: dir = float3(-1.0, -st.y, st.x); break;
    case 2: dir = float3(st.x, 1.0, st.y); break;
    case 3: dir = float3(st.x, -1.0, -st.y); break;
    case 4: dir = float3(st.x, -st.y, 1.0); break;
    default: dir = float3(-st.x, -st.y, -1.0); break;
    }
    return normalize(dir);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.FaceSize || id.y >= g_PC.FaceSize)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float(g_PC.FaceSize);
    float3 normal = FaceDirection(id.z, uv);

    // Tangent basis around the normal for the hemisphere walk.
    float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));

    float3 irradiance = float3(0.0);
    uint sampleCount = 0;
    const float delta = 0.025;
    for (float phi = 0.0; phi < 2.0 * Pi; phi += delta)
    {
        for (float theta = 0.0; theta < 0.5 * Pi; theta += delta)
        {
            // Spherical sample in tangent space, transformed to world.
            float3 tangentSample =
                float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleDir =
                tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal;

            irradiance += g_Env.SampleLevel(g_Sampler, sampleDir, 0.0).rgb * cos(theta) * sin(theta);
            sampleCount++;
        }
    }

    irradiance = Pi * irradiance / float(max(sampleCount, 1u));
    g_Dest[uint3(id.xy, id.z)] = float4(irradiance, 1.0);
}
