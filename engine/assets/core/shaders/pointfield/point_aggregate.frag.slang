// Point-field aggregate-splat fragment stage. A separable quadratic B-spline over the quad's
// three-cell support, additively blended: neighboring aggregated cells' kernels sum to one
// everywhere (a partition of unity over the cull lattice), so a uniformly dense region reads as
// a flat field with no imprint of the cull grid. Unlit and depth-unaware: the aggregate is a
// coarse far/dense representation where per-point occlusion no longer reads, so it composites as
// pure emissive glow.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_Color : COLOR0;
};

// The quadratic B-spline in cell-width units: support [-1.5, 1.5], and its integer-offset
// translates sum to one everywhere — the property that erases the lattice from the summed field.
float BSpline2(float t)
{
    float a = abs(t);
    if (a <= 0.5)
    {
        return 0.75 - a * a;
    }
    if (a < 1.5)
    {
        float s = 1.5 - a;
        return 0.5 * s * s;
    }
    return 0.0;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    // The quad spans three cell widths, so uv in [-0.5, 0.5] maps to [-1.5, 1.5] cell units.
    float2 t = input.v_UV * 3.0;
    float falloff = BSpline2(t.x) * BSpline2(t.y);
    return float4(input.v_Color * falloff, 1.0);
}
