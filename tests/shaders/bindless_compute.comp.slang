// Reads a texture through the bindless 2D array with a bindless sampler, and a word through the
// bindless storage-buffer array, both selected by pushed handles, and writes them to a set-3 buffer.

#include "Veng/bindless.slang"

struct BindlessComputeSet3
{
    RWStructuredBuffer<float4> Output;
};
VE_DESCRIPTOR_SET(3, BindlessComputeSet3, g_Set3);

struct BindlessComputePush
{
    uint Texture;
    uint Sampler;
    uint Buffer;
    uint Pad;
};
VE_PUSH_CONSTANTS(BindlessComputePush, u_Push);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    g_Set3.Output[0] = g_Textures[NonUniformIndex(u_Push.Texture)].SampleLevel(
        g_Samplers[NonUniformIndex(u_Push.Sampler)], float2(0.5, 0.5), 0.0);
    g_Set3.Output[1] = asfloat(g_Buffers[NonUniformIndex(u_Push.Buffer)].Load4(16));
}
