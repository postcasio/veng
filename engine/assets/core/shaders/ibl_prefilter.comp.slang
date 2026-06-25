// Pre-filtered specular environment: GGX importance-sampled convolution of the radiance
// cube, one dispatch per roughness mip (the renderer pushes the mip's roughness). The
// result is the split-sum specular prefilter the lighting pass samples by reflection
// vector at a roughness-derived LOD.
//
// Source radiance cube and destination prefilter mip both bind on set 1.
[[vk::binding(0, 1)]] TextureCube g_Env;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2DArray<float4> g_Dest;

struct PushConstants
{
    uint FaceSize;     // destination mip face edge length in texels
    float Roughness;   // this mip's roughness in [0,1]
    uint SampleCount;  // importance-sample count
    float SourceSize;  // source cube face edge length (for the mip-bias filter)
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

// Van der Corput radical inverse + Hammersley point set for low-discrepancy sampling.
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

// GGX importance-sampled half-vector around the normal for roughness.
float3 ImportanceSampleGGX(float2 xi, float3 normal, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * Pi * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

float DistributionGGX(float nDotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(Pi * d * d, 1e-7);
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

    float3 prefiltered = float3(0.0);
    float totalWeight = 0.0;
    for (uint i = 0; i < g_PC.SampleCount; ++i)
    {
        float2 xi = Hammersley(i, g_PC.SampleCount);
        float3 h = ImportanceSampleGGX(xi, normal, g_PC.Roughness);
        float3 lightDir = normalize(2.0 * dot(viewDir, h) * h - viewDir);

        float nDotL = dot(normal, lightDir);
        if (nDotL > 0.0)
        {
            // Sample a pre-filtered mip of the source proportional to the solid angle the
            // sample covers, suppressing fireflies (Krivanek/Colbert).
            float nDotH = max(dot(normal, h), 0.0);
            float hDotV = max(dot(h, viewDir), 0.0);
            float d = DistributionGGX(nDotH, g_PC.Roughness);
            float pdf = (d * nDotH / (4.0 * hDotV)) + 1e-4;

            float saTexel = 4.0 * Pi / (6.0 * g_PC.SourceSize * g_PC.SourceSize);
            float saSample = 1.0 / (float(g_PC.SampleCount) * pdf + 1e-4);
            float mipBias = g_PC.Roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefiltered += g_Env.SampleLevel(g_Sampler, lightDir, mipBias).rgb * nDotL;
            totalWeight += nDotL;
        }
    }

    prefiltered = totalWeight > 0.0 ? prefiltered / totalWeight : prefiltered;
    g_Dest[uint3(id.xy, id.z)] = float4(prefiltered, 1.0);
}
