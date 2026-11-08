// The solid-angle sums of a layered sky bake's field: one workgroup per face of one field target
// weights every mip-0 texel by the exact solid angle it covers and writes the face's sum, so
// SkyBakeFieldMean (Veng/sky_bake.slang) is the six sums over the sphere's 4π. A box mip chain
// weights every texel alike, while a cube texel's solid angle varies about fivefold across a face.

#include "Veng/binding.slang"
struct SkyBakeFieldSumSet3
{
    Texture2DArray<float4> Field;
    RWStructuredBuffer<float4> Sums;
};
VE_DESCRIPTOR_SET(3, SkyBakeFieldSumSet3, g_Set3);

struct PushConstants
{
    uint FaceSize;    // the field's face edge in texels
    uint OutputBase;  // the first of this target's six sums
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

static const uint GroupSize = 256;

groupshared float4 s_Partial[GroupSize];

// The solid angle the face rectangle from the face centre to (x, y), in [-1, 1] face coordinates,
// subtends; a texel's is the inclusion-exclusion of its four corners.
float CornerSolidAngle(float x, float y)
{
    return atan2(x * y, sqrt(x * x + y * y + 1.0));
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint face = group.z;
    const uint size = g_PC.FaceSize;
    const float texel = 2.0 / float(size);

    float4 sum = float4(0.0);
    for (uint index = thread.x; index < size * size; index += GroupSize)
    {
        const uint x = index % size;
        const uint y = index / size;
        const float x0 = float(x) * texel - 1.0;
        const float y0 = float(y) * texel - 1.0;
        const float x1 = x0 + texel;
        const float y1 = y0 + texel;
        const float solidAngle = CornerSolidAngle(x1, y1) - CornerSolidAngle(x0, y1) -
                                 CornerSolidAngle(x1, y0) + CornerSolidAngle(x0, y0);
        sum += solidAngle * g_Set3.Field.Load(int4(int(x), int(y), int(face), 0));
    }

    s_Partial[thread.x] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = GroupSize / 2; stride > 0; stride /= 2)
    {
        if (thread.x < stride)
        {
            s_Partial[thread.x] += s_Partial[thread.x + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread.x == 0)
    {
        g_Set3.Sums[g_PC.OutputBase + face] = s_Partial[0];
    }
}
