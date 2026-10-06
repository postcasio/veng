// Pre-filtered specular environment: GGX importance-sampled convolution of the radiance
// cube, one dispatch per roughness mip (the renderer pushes the mip's roughness). The
// result is the split-sum specular prefilter the lighting pass samples by reflection
// vector at a roughness-derived LOD. The GGX terms are the lighting core's (Veng/brdf.slang).
//
// Source radiance cube and destination prefilter mip both bind on set 3.
#include "Veng/brdf.slang"

[[vk::binding(0, 3)]] TextureCube g_Env;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2DArray<float4> g_Dest;

struct PushConstants
{
    uint FaceSize;     // destination mip face edge length in texels
    float Roughness;   // this mip's roughness in [0,1]
    uint SampleCount;  // importance-sample count
    float SourceSize;  // source cube face edge length (for the mip-bias filter)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

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

    // The split-sum approximation assumes V == R == N.
    float3 reflectDir = normal;
    float3 viewDir = normal;

    float alpha = g_PC.Roughness * g_PC.Roughness;
    float3 prefiltered = float3(0.0);
    float totalWeight = 0.0;
    for (uint i = 0; i < g_PC.SampleCount; ++i)
    {
        float2 xi = BrdfHammersley(i, g_PC.SampleCount);
        float3 h = BrdfImportanceSampleGGX(xi, normal, alpha);
        float3 lightDir = normalize(2.0 * dot(viewDir, h) * h - viewDir);

        float nDotL = dot(normal, lightDir);
        if (nDotL > 0.0)
        {
            // Sample a pre-filtered mip of the source proportional to the solid angle the
            // sample covers, suppressing fireflies (Krivanek/Colbert). The mirror mip reads the
            // source's own level: its every sample is h = normal, where D is 0/0.
            float mipBias = 0.0;
            if (g_PC.Roughness > 0.0)
            {
                float nDotH = max(dot(normal, h), 0.0);
                float hDotV = max(dot(h, viewDir), 0.0);
                float d = BrdfDistributionGGX(normal, h, nDotH, alpha);
                float pdf = (d * nDotH / (4.0 * hDotV)) + 1e-4;

                float saTexel = 4.0 * BrdfPi / (6.0 * g_PC.SourceSize * g_PC.SourceSize);
                float saSample = 1.0 / (float(g_PC.SampleCount) * pdf + 1e-4);
                mipBias = 0.5 * log2(saSample / saTexel);
            }

            prefiltered += g_Env.SampleLevel(g_Sampler, lightDir, mipBias).rgb * nDotL;
            totalWeight += nDotL;
        }
    }

    prefiltered = totalWeight > 0.0 ? prefiltered / totalWeight : prefiltered;
    g_Dest[uint3(id.xy, id.z)] = float4(prefiltered, 1.0);
}
