// Counts each fragment it shades and records which candidate drew it, so a one-pixel target counts
// the instances an indirect draw executed and names the candidates among them.
#include "Veng/binding.slang"

struct IndirectCountSeenSet3
{
    RWStructuredBuffer<uint> Seen; // [0] fragments shaded, [1] a bit per candidate drawn
};
VE_DESCRIPTOR_SET(3, IndirectCountSeenSet3, g_Set3);

struct VSOutput
{
    float4 sv_position : SV_Position;
    nointerpolation uint v_CandidateId : CANDIDATE0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    InterlockedAdd(g_Set3.Seen[0], 1u);
    InterlockedOr(g_Set3.Seen[1], 1u << input.v_CandidateId);
    return float4(1.0, 1.0, 1.0, 1.0);
}
