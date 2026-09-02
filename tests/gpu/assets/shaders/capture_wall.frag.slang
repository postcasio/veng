#include "Veng/surface.slang"

// An opaque, depth-writing, double-sided wall (gpu fixture): emits a fixed authored Color and writes
// depth, so a capture centred among several of these records their distances. Depth-writing is the
// point — the octahedral distance map resamples the face depths, so a wall must be opaque to appear
// in it — and a fixed colour lets the radiance-map registration case tell one wall from another.
struct MaterialParams
{
    float4 Color; // Emitted radiance (linear HDR).
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    GBufferOutput output;
    output.Albedo = float4(0.0, 0.0, 0.0, 1.0);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = p.Color.rgb;
    return output;
}
