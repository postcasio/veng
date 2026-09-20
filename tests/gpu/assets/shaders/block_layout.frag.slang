// Parameter-block layout fixture (gpu). Its MaterialParams is shaped to break a packer that
// 16-aligns vectors: a scalar leads, so the float3 behind it sits at offset 4, and the float2
// behind the uint sits at offset 36 — neither 16-aligned. The cooker packs the block on a tight
// 4-byte cursor and the shader reads it back through Load<MaterialParams>, so any disagreement
// between the two surfaces as a member reading a neighbour's bytes.
//
// The fragment writes one member component per vertical band of the output, so a single
// fullscreen draw carries every scalar in the block back to the host.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float  Lead;   // 0
    float3 Mid;    // 4
    float4 Region; // 16
    uint   Tag;    // 32
    float2 Tail;   // 36
};

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p =
        g_MaterialParams.Load<MaterialParams>(g_PC.MaterialIndex * MaterialParamStride);

    float members[11] = {
        p.Lead,
        p.Mid.x,      p.Mid.y,      p.Mid.z,
        p.Region.x,   p.Region.y,   p.Region.z,   p.Region.w,
        float(p.Tag),
        p.Tail.x,     p.Tail.y
    };

    uint band = min(uint(input.v_UV.x * 11.0), 10u);
    return float4(members[band], 0.0, 0.0, 1.0);
}
