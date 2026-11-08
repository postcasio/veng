// The two mismatching layouts rewritten so both targets agree, keeping every SPIR-V offset: the
// float3 and the scalar after it share a float4, and the float2 is padded out before the float3,
// whose element is padded to a whole 16 bytes.
#include "Veng/binding.slang"

struct Light
{
    float4 DirectionScale;
};

struct Point
{
    float2 Uv;
    float2 Pad0;
    float3 Position;
    float Pad1;
};

struct LayoutSet
{
    ConstantBuffer<Light> Sun;
    StructuredBuffer<Point> Points;
    RWStructuredBuffer<float4> Out;
};
VE_DESCRIPTOR_SET(3, LayoutSet, g_Layout);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const Point p = g_Layout.Points[id.x];
    g_Layout.Out[id.x] =
        float4(g_Layout.Sun.DirectionScale.xyz * g_Layout.Sun.DirectionScale.w + p.Position,
               p.Uv.x);
}
