#include "Veng/surface.slang"

// Opaque metallic-roughness fixture (gpu): writes its authored base colour, roughness, metallic and
// occlusion straight into the g-buffer, so the deferred lighting pass shades exactly the parameters
// the forward-lit translucent twin (forward_lit.frag) shades itself with.
struct MaterialParams
{
    float4 BaseColor; // linear rgb; a unused
    float  Roughness;
    float  Metallic;
    float  Occlusion;
    float  Coverage;  // unused by the opaque twin
};

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);

    GBufferOutput output;
    output.Albedo = float4(p.BaseColor.rgb, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(p.Occlusion, p.Roughness, p.Metallic, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
