// BRDF integration LUT: the environment-independent half of the split-sum approximation.
// Each texel (x = NdotV, y = roughness) stores the GGX scale/bias pair (A, B) the lighting
// pass combines with the Fresnel term: specular = prefiltered * (F0 * A + B). Generated
// once at renderer construction. The GGX sampler and the image-based Smith term are the lighting
// core's (Veng/brdf.slang).
#include "Veng/brdf.slang"

[[vk::binding(0, 3)]] [format("rg16f")] RWTexture2D<float2> g_Dest;

struct PushConstants
{
    uint Size; // LUT edge length in texels
    uint SampleCount;
    uint Pad0;
    uint Pad1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.Size || id.y >= g_PC.Size)
        return;

    float nDotV = (float(id.x) + 0.5) / float(g_PC.Size);
    float roughness = (float(id.y) + 0.5) / float(g_PC.Size);

    float3 viewDir = float3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
    float3 normal = float3(0.0, 0.0, 1.0);

    float a = 0.0;
    float b = 0.0;
    for (uint i = 0; i < g_PC.SampleCount; ++i)
    {
        float2 xi = BrdfHammersley(i, g_PC.SampleCount);
        float3 h = BrdfImportanceSampleGGX(xi, normal, roughness * roughness);
        float3 lightDir = normalize(2.0 * dot(viewDir, h) * h - viewDir);

        float nDotL = max(lightDir.z, 0.0);
        float nDotH = max(h.z, 0.0);
        float vDotH = max(dot(viewDir, h), 0.0);

        if (nDotL > 0.0)
        {
            float g = BrdfGeometrySmithIbl(nDotV, nDotL, roughness);
            float gVis = (g * vDotH) / (nDotH * nDotV);
            float fc = pow(1.0 - vDotH, 5.0);

            a += (1.0 - fc) * gVis;
            b += fc * gVis;
        }
    }

    a /= float(g_PC.SampleCount);
    b /= float(g_PC.SampleCount);
    g_Dest[int2(id.xy)] = float2(a, b);
}
