// A constant block whose scalar follows a float3: Metal pads the float3 to 16 bytes, so the scalar
// sits 4 bytes later than on SPIR-V.
#include "Veng/binding.slang"

struct Light
{
    float3 Direction;
    float Scale;
};

struct LayoutSet
{
    ConstantBuffer<Light> Sun;
    RWStructuredBuffer<float4> Out;
};
VE_DESCRIPTOR_SET(3, LayoutSet, g_Layout);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    g_Layout.Out[id.x] = float4(g_Layout.Sun.Direction * g_Layout.Sun.Scale, 1.0);
}
