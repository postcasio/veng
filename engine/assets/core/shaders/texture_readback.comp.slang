// Texel readback: fetches every texel of one mip level of a 2D or layered image — a cube is read as
// its six layers — as float RGBA into a storage buffer, layer-major then row-major. A texel fetch
// decodes whatever the image stores (sRGB, block-compressed, packed float), so one pipeline reads
// any sampleable colour format, and the image needs no transfer usage.

#include "Veng/binding.slang"
struct TextureReadbackSet3
{
    Texture2DArray<float4> Source;
    RWStructuredBuffer<float4> Texels;
};
VE_DESCRIPTOR_SET(3, TextureReadbackSet3, g_Set3);

struct PushConstants
{
    uint2 Extent; // the level's extent in texels
    uint  Layers; // array layers read
    uint  Pad;
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.Extent.x || id.y >= g_PC.Extent.y || id.z >= g_PC.Layers)
    {
        return;
    }
    const uint index = (id.z * g_PC.Extent.y + id.y) * g_PC.Extent.x + id.x;
    g_Set3.Texels[index] = g_Set3.Source.Load(int4(int2(id.xy), int(id.z), 0));
}
