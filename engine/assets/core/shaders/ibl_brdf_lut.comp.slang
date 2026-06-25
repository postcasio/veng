// BRDF integration LUT: the environment-independent half of the split-sum approximation.
// Each texel (x = NdotV, y = roughness) stores the GGX scale/bias pair (A, B) the lighting
// pass combines with the Fresnel term: specular = prefiltered * (F0 * A + B). Generated
// once at renderer construction.
[[vk::binding(0, 1)]] [format("rg16f")] RWTexture2D<float2> g_Dest;

struct PushConstants
{
    uint Size; // LUT edge length in texels
    uint SampleCount;
    uint Pad0;
    uint Pad1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

static const float Pi = 3.14159265358979323846;

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

// Smith geometry with the IBL k (a = roughness^2 / 2).
float GeometrySchlickGGX(float nDotV, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    return nDotV / (nDotV * (1.0 - k) + k);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    float nDotV = max(dot(normal, viewDir), 0.0);
    float nDotL = max(dot(normal, lightDir), 0.0);
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

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
        float2 xi = Hammersley(i, g_PC.SampleCount);
        float3 h = ImportanceSampleGGX(xi, normal, roughness);
        float3 lightDir = normalize(2.0 * dot(viewDir, h) * h - viewDir);

        float nDotL = max(lightDir.z, 0.0);
        float nDotH = max(h.z, 0.0);
        float vDotH = max(dot(viewDir, h), 0.0);

        if (nDotL > 0.0)
        {
            float g = GeometrySmith(normal, viewDir, lightDir, roughness);
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
