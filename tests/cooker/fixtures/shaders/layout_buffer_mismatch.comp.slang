// A structured-buffer element with a float3 after a float2: Metal packs the float3 at offset 8,
// where std430 aligns it to 16.
#include "Veng/binding.slang"

struct Point
{
    float2 Uv;
    float3 Position;
};

struct LayoutSet
{
    StructuredBuffer<Point> Points;
    RWStructuredBuffer<float4> Out;
};
VE_DESCRIPTOR_SET(3, LayoutSet, g_Layout);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const Point p = g_Layout.Points[id.x];
    g_Layout.Out[id.x] = float4(p.Position, p.Uv.x);
}
