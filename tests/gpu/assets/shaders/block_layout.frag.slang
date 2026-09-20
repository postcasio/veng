// Parameter-block layout fixture (gpu). Its MaterialParams is shaped to break a packer that
// 16-aligns vectors: a scalar leads, so the float3 behind it sits at offset 4, and the float2
// behind the uint sits at offset 36 — neither 16-aligned. The cooker packs the block on a tight
// 4-byte cursor and the shader reads it back through Load<MaterialParams>, so any disagreement
// between the two surfaces as a member reading a neighbour's bytes.
//
// The tail is the array and nested-struct half, which is a distinct layout question: an array's
// *element* stride is not the member packing above. Tri starts at 44 (not 16-aligned) and strides
// 12, Scalars strides 4, Quads starts at 92 (not 16-aligned) and strides 16, and Bands is an array
// of a struct, which flattens per element into dotted members. A packer that strode an array by 16
// would read every element but the first from the wrong place.
//
// The fragment writes one member component per vertical band of the output, so a single
// fullscreen draw carries every scalar in the block back to the host.

#include "Veng/postprocess.slang"

struct Band
{
    float2 Low;
    float  Weight;
};

struct MaterialParams
{
    float  Lead;      //   0
    float3 Mid;       //   4
    float4 Region;    //  16
    uint   Tag;       //  32
    float2 Tail;      //  36
    float3 Tri[3];    //  44, stride 12 → 44, 56, 68
    float  Scalars[3];//  80, stride  4 → 80, 84, 88
    float4 Quads[2];  //  92, stride 16 → 92, 108
    Band   Bands[2];  // 124, stride 12 → 124, 136
};                    // 148 bytes

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p =
        g_MaterialParams.Load<MaterialParams>(g_PC.MaterialIndex);

    float members[37] = {
        p.Lead,
        p.Mid.x,      p.Mid.y,      p.Mid.z,
        p.Region.x,   p.Region.y,   p.Region.z,   p.Region.w,
        float(p.Tag),
        p.Tail.x,     p.Tail.y,
        p.Tri[0].x,   p.Tri[0].y,   p.Tri[0].z,
        p.Tri[1].x,   p.Tri[1].y,   p.Tri[1].z,
        p.Tri[2].x,   p.Tri[2].y,   p.Tri[2].z,
        p.Scalars[0], p.Scalars[1], p.Scalars[2],
        p.Quads[0].x, p.Quads[0].y, p.Quads[0].z, p.Quads[0].w,
        p.Quads[1].x, p.Quads[1].y, p.Quads[1].z, p.Quads[1].w,
        p.Bands[0].Low.x, p.Bands[0].Low.y, p.Bands[0].Weight,
        p.Bands[1].Low.x, p.Bands[1].Low.y, p.Bands[1].Weight
    };

    uint band = min(uint(input.v_UV.x * 37.0), 36u);
    return float4(members[band], 0.0, 0.0, 1.0);
}
