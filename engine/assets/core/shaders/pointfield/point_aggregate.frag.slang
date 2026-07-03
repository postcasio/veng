// Point-field aggregate-splat fragment stage. A soft radial glow over the splat quad, additively
// blended so overlapping cell splats sum into a continuous density field. Unlit and depth-
// unaware: the aggregate is a coarse far/dense representation where per-point occlusion no longer
// reads, so it composites as pure emissive glow.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_Color : COLOR0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    // Gaussian-ish radial falloff over the [-0.5,0.5] quad.
    float r = length(input.v_UV) * 2.0;
    float falloff = saturate(1.0 - r);
    falloff *= falloff;
    return float4(input.v_Color * falloff, 1.0);
}
