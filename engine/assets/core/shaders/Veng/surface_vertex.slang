// The shared surface vertex stage: the DrawData/view fetch plumbing (from surface.slang) and the
// world/normal-matrix transform + output-assembly that turns an object-space vertex into the
// SurfaceFragmentInput a Surface fragment reads. The static (surface.vert), skinned
// (surface_skinned.vert) and instanced-set (surface_instanced.vert) vertex shaders all include this
// and differ only in how they produce the pre-transform object-space position/normal/tangent and
// the model matrix they hand to the assembly — the static stage passes the canonical attributes
// through, the skinned stage passes the palette-blended result, the instanced stage composes the
// instance's transform into the set's. The fragment-input struct (SurfaceFragmentInput) and the
// DrawData / view / push-constant contract live in surface.slang, so there is one output struct and
// one transform across every stage: a drift between them is impossible by construction.
#include "surface.slang"

// Assembles the surface fragment-input interpolants from an object-space vertex placed by an explicit
// model matrix, its previous-frame twin, and the three columns of its normal matrix (the
// inverse-transpose of the model matrix's upper 3x3). The caller supplies the object-space position
// (post-skinning for a skinned mesh), normal, tangent and UV, plus the previous-frame object-space
// position. The candidate id is passed through so a fragment can reload the draw's DrawData
// (LoadDrawData); the instance interpolant reads InstanceNone, which an instanced stage overwrites.
SurfaceFragmentInput AssembleSurfaceVertexWith(float4x4 world, float4x4 prevWorld, float3 c0,
                                               float3 c1, float3 c2, uint materialOffset,
                                               uint candidateId, ViewConstants view, float4 objPos,
                                               float3 objNormal, float4 objTangent, float2 uv,
                                               float4 prevObjPos)
{
    float4 worldPos = mul(world, objPos);
    float4 prevWorldPos = mul(prevWorld, prevObjPos);

    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    // The motion vector is measured in unjittered clip space so the jitter the TAA
    // resolve is integrating does not leak into the reprojection.
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = uv;
    // Transforming a vector by the normal matrix is the column-weighted sum, matching the
    // column-major glm matrix the renderer uploads.
    output.v_WorldNormal = c0 * objNormal.x + c1 * objNormal.y + c2 * objNormal.z;
    // NormalMatrix carries the tangent to world space alongside the normal; the
    // canonical tangent's w is the bitangent handedness sign, passed through.
    output.v_WorldTangent =
        float4(c0 * objTangent.x + c1 * objTangent.y + c2 * objTangent.z, objTangent.w);
    output.v_MaterialOffset = materialOffset;
    output.v_CandidateId = candidateId;
    output.v_Instance = uint2(InstanceNone, 0u);
    output.v_ObjectPosition = objPos.xyz;
    return output;
}

// Assembles the surface fragment-input interpolants from an object-space vertex, applying the
// per-draw World / PrevWorld and normal matrix (AssembleSurfaceVertexWith over the draw's record).
// The previous-frame object-space position equals the current position for a static mesh (only
// World/PrevWorld move it) and is the previous-frame deformation for a skinned one.
SurfaceFragmentInput AssembleSurfaceVertex(DrawData draw, uint candidateId, ViewConstants view,
                                           float4 objPos, float3 objNormal, float4 objTangent,
                                           float2 uv, float4 prevObjPos)
{
    return AssembleSurfaceVertexWith(draw.World, draw.PrevWorld, draw.NormalColumn0.xyz,
                                     draw.NormalColumn1.xyz, draw.NormalColumn2.xyz,
                                     draw.MaterialOffset, candidateId, view, objPos, objNormal,
                                     objTangent, uv, prevObjPos);
}
