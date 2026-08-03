// The shared surface vertex stage: the DrawData/view fetch plumbing (from surface.slang) and the
// world/normal-matrix transform + output-assembly that turns an object-space vertex into the
// SurfaceFragmentInput a Surface fragment reads. The static (surface.vert) and skinned
// (surface_skinned.vert) vertex shaders both include this and differ only in how they produce the
// pre-transform object-space position/normal/tangent they hand to AssembleSurfaceVertex — the
// static stage passes the canonical attributes through, the skinned stage passes the
// palette-blended result. The fragment-input struct (SurfaceFragmentInput) and the DrawData / view
// / push-constant contract live in surface.slang, so there is one output struct and one transform
// across both stages: a static/skinned drift is impossible by construction.
#include "surface.slang"

// Assembles the surface fragment-input interpolants from an object-space vertex, applying the
// per-draw World / PrevWorld and the view projection. The caller supplies the object-space
// position (pre-World, post-skinning for a skinned mesh), normal, tangent, and UV, plus the
// previous-frame object-space position — equal to the current position for a static mesh (only
// World/PrevWorld move it), the previous-frame deformation for a skinned one. The candidate id is
// passed through to the fragment so a fragment can reload this draw's DrawData (LoadDrawData).
SurfaceFragmentInput AssembleSurfaceVertex(DrawData draw, uint candidateId, ViewConstants view,
                                           float4 objPos, float3 objNormal, float4 objTangent,
                                           float2 uv, float4 prevObjPos)
{
    // The normal matrix (inverse-transpose of World's upper 3x3) is carried as its
    // three columns; transforming a vector is the column-weighted sum, matching the
    // column-major glm matrix the renderer uploads.
    float3 c0 = draw.NormalColumn0.xyz;
    float3 c1 = draw.NormalColumn1.xyz;
    float3 c2 = draw.NormalColumn2.xyz;

    float4 worldPos = mul(draw.World, objPos);
    float4 prevWorldPos = mul(draw.PrevWorld, prevObjPos);

    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    // The motion vector is measured in unjittered clip space so the jitter the TAA
    // resolve is integrating does not leak into the reprojection.
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = uv;
    output.v_WorldNormal = c0 * objNormal.x + c1 * objNormal.y + c2 * objNormal.z;
    // NormalMatrix carries the tangent to world space alongside the normal; the
    // canonical tangent's w is the bitangent handedness sign, passed through.
    output.v_WorldTangent =
        float4(c0 * objTangent.x + c1 * objTangent.y + c2 * objTangent.z, objTangent.w);
    output.v_MaterialIndex = draw.MaterialIndex;
    output.v_CandidateId = candidateId;
    return output;
}
