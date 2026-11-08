// One of each binding shape the Metal binding model places: the three bindless sets, a typed set
// holding a comparison sampler, a storage image, a 3D texture, a combined image sampler, an arrayed
// binding and a structured buffer, a dynamic uniform buffer, a push-constant block, a resource
// index that may differ per invocation, and Wave operations.
#include "Veng/bindless.slang"

struct Constants
{
    float4 Tint;
};

struct ModelSet
{
    Texture2DArray<float> ShadowMap;
    SamplerComparisonState Compare;
    [format("rgba16f")] RWTexture2D<float4> Output;
    Texture3D<float> Volume;
    Sampler2D Combined;
    Texture2D Many[4];
    StructuredBuffer<float4> Data;
};
VE_DESCRIPTOR_SET(3, ModelSet, g_Model);
VE_DYNAMIC_UNIFORM_BUFFER(7, 3, 0, Constants, g_Constants);

struct Push
{
    uint Texture;
    uint Sampler;
    uint Volume;
    uint Cube;
};
VE_PUSH_CONSTANTS(Push, g_PC);

[shader("compute")]
[numthreads(32, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    float4 value = g_Textures[NonUniformIndex(g_PC.Texture + id.x)].SampleLevel(
        g_Samplers[g_PC.Sampler], float2(0.5, 0.5), 0.0);
    value += g_Volumes[g_PC.Volume].SampleLevel(g_Samplers[g_PC.Sampler], float3(0.5, 0.5, 0.5), 0.0);
    value += g_Cubes[g_PC.Cube].SampleLevel(g_Samplers[g_PC.Sampler], float3(1.0, 0.0, 0.0), 0.0);
    value += g_Model.ShadowMap.SampleCmpLevelZero(g_Model.Compare, float3(0.5, 0.5, 0.0), 0.5);
    value += g_Model.Volume.Load(int4(0, 0, 0, 0));
    value += g_Model.Combined.SampleLevel(float2(0.5, 0.5), 0.0);
    for (uint i = 0; i < 4; ++i)
    {
        value += g_Model.Many[i].Load(int3(0, 0, 0));
    }
    value += g_Model.Data[id.x] + g_Constants.Tint;
    value.x += WaveActiveSum(value.x);
    value.y = WavePrefixSum(value.y);
    value.z = WaveReadLaneFirst(value.z);
    g_Model.Output[id.xy] = value;
}
