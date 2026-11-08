// Inverts an RGBA8 image: writes (1 - rgb, a) of the source storage image into
// the destination storage image, one texel per invocation. Sets 0-2 are reserved
// for the bindless registries, so the storage images bind on set 3; the test's
// hand-built layout matches these bindings.

#include "Veng/binding.slang"
struct InvertSet3
{
    [format("rgba8")] RWTexture2D<float4> Source;
    [format("rgba8")] RWTexture2D<float4> Destination;
};
VE_DESCRIPTOR_SET(3, InvertSet3, g_Set3);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    int2 coord = int2(id.xy);
    float4 source = g_Set3.Source[coord];
    g_Set3.Destination[coord] = float4(float3(1.0) - source.rgb, source.a);
}
