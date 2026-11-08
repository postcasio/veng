// Samples a combined image sampler at the interpolated UV. Sets 0-2 are reserved
// for the bindless registries, so the sampler binds on set 3, matching the test's
// hand-built CombinedImageSampler layout.

#include "Veng/binding.slang"
struct SampleSet3
{
    Sampler2D Source;
};
VE_DESCRIPTOR_SET(3, SampleSet3, g_Set3);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return g_Set3.Source.Sample(input.v_UV);
}
