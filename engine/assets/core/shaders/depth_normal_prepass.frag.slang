// Fragment stage of the lean geometry render: writes the interpolated world-space normal,
// renormalized, into the one-MRT world-normal target — no shading and no material sample. The
// depth attachment carries the silhouette; this channel carries the surface orientation, in the
// same signed world-space convention as the g-buffer normal (G1), so a consumer sobels depth and
// normal into an outline. A background texel keeps the target's zero clear.
struct FSInput
{
    float4 sv_position : SV_Position;
    float3 v_WorldNormal : TEXCOORD0;
};

struct FSOutput
{
    float4 Normal : SV_Target0;
};

[shader("fragment")]
FSOutput fsMain(FSInput input)
{
    FSOutput output;
    output.Normal = float4(normalize(input.v_WorldNormal), 1.0);
    return output;
}
