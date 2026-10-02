#include "Veng/surface.slang"

// The g-buffer shading override's fragment stage (SceneRendererSettings::GBufferShadingOverride).
// The renderer pairs it with each opaque draw's own vertex stage, layout and face culling, so the
// draw rasterizes the same geometry and only this stage replaces the material's shading: a constant
// albedo, the interpolated vertex normal, and no material block or texture read. The motion vector
// is still written so a temporal resolve reprojects the frame as it would any other.
[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    GBufferOutput output;
    output.Albedo = float4(0.5, 0.5, 0.5, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    // Unoccluded, mid roughness, dielectric, no surface flags.
    output.ORM = float4(1.0, 0.5, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0, 0.0, 0.0);
    return output;
}
