// A set whose combined sampler the stage never samples. Slang emits that field on Metal as a lone
// texture, one argument-buffer element where the binding model places two, so every field after it
// would bind one element early; the cook refuses it.
#include "Veng/binding.slang"

struct UnsampledSet
{
    Sampler2D Unsampled;
    Texture2D<float4> Source;
    SamplerState Linear;
    [format("rgba16f")] RWTexture2D<float4> Output;
};
VE_DESCRIPTOR_SET(3, UnsampledSet, g_Set);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    g_Set.Output[id.xy] = g_Set.Source.SampleLevel(g_Set.Linear, float2(0.5, 0.5), 0.0);
}
