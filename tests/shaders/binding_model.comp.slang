// Reads one value through each shape of binding the Metal binding model places differently — a
// combined image sampler (two argument-buffer elements), a uniform buffer and a storage buffer in a
// set, a dynamic uniform buffer bound by address at its own slot, and two push-constant ranges
// pushed apart — and writes each to its own output element.

#include "Veng/binding.slang"

struct BindingParams
{
    float4 Value;
};

struct BindingDynamic
{
    float4 Value;
};

struct BindingModelSet3
{
    Sampler2D<float4> Image;
    ConstantBuffer<BindingParams> Params;
    RWStructuredBuffer<float4> Output;
};
VE_DESCRIPTOR_SET(3, BindingModelSet3, g_Set3);
VE_DYNAMIC_UNIFORM_BUFFER(3, 3, 0, BindingDynamic, g_Dynamic);

struct BindingModelPush
{
    float4 First;
    float4 Second;
};
VE_PUSH_CONSTANTS(BindingModelPush, u_Push);

[shader("compute")]
[numthreads(8, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    switch (id.x)
    {
    case 0:
        g_Set3.Output[0] = g_Set3.Image.SampleLevel(float2(0.5, 0.5), 0.0);
        break;
    case 1:
        g_Set3.Output[1] = g_Set3.Params.Value;
        break;
    case 2:
        g_Set3.Output[2] = g_Dynamic.Value;
        break;
    case 3:
        g_Set3.Output[3] = u_Push.First;
        break;
    case 4:
        g_Set3.Output[4] = u_Push.Second;
        break;
    default:
        break;
    }
}
