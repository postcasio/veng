#include "Veng/surface_vertex.slang"
#include "Veng/instanced_mesh_math.slang"

// Deferred surface vertex stage for an instanced mesh set. VSInput is veng's canonical vertex
// layout on binding 0 with no instance-rate binding: the draw's firstInstance is the start of a
// level's survivor list in the frame's instance buffer, so the vertex's instance index
// (SV_VulkanInstanceID, which counts from firstInstance) names its list entry, and the entry names
// the instance's record. The set's own record in g_DrawData (push InstanceDraw) carries the set's
// frame and material; each vertex composes the instance's transform into it.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
};

// The columns of the normal matrix (the inverse-transpose of the model matrix's upper 3x3), from
// its cofactors: correct under non-uniform and mirrored scale, at three cross products a vertex.
void NormalColumns(float4x4 model, out float3 c0, out float3 c1, out float3 c2)
{
    const float3 m0 = float3(model[0][0], model[1][0], model[2][0]);
    const float3 m1 = float3(model[0][1], model[1][1], model[2][1]);
    const float3 m2 = float3(model[0][2], model[1][2], model[2][2]);
    const float3 x12 = cross(m1, m2);
    const float invDet = 1.0 / dot(m0, x12);
    // inverse(M)^T's column j is the cofactor column of M's column j, over the determinant.
    c0 = x12 * invDet;
    c1 = cross(m2, m0) * invDet;
    c2 = cross(m0, m1) * invDet;
}

[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input, uint listIndex : SV_VulkanInstanceID)
{
    const ByteAddressBuffer instances = g_Buffers[g_PC.InstanceBuffer];
    const uint instance = instances.Load(listIndex * 4u);
    const uint recordIndex = g_PC.InstanceBase + instance;
    const InstanceRecord record =
        instances.Load<InstanceRecord>(recordIndex * InstanceRecordBytes);

    const DrawData draw = LoadDrawData(g_PC.InstanceDraw);
    const ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    const float4x4 local = InstanceMatrix(record);
    const float4x4 world = mul(draw.World, local);
    // The instance's own transform is this frame's; only the set's frame moves it in velocity.
    const float4x4 prevWorld = mul(draw.PrevWorld, local);
    float3 c0;
    float3 c1;
    float3 c2;
    NormalColumns(world, c0, c1, c2);

    const float4 objPos = float4(input.a_Position, 1.0);
    SurfaceFragmentInput output =
        AssembleSurfaceVertexWith(world, prevWorld, c0, c1, c2, draw.MaterialOffset,
                                  g_PC.InstanceDraw, view, objPos, input.a_Normal, input.a_Tangent,
                                  input.a_UV, objPos);
    output.v_Instance = uint2(g_PC.InstanceBuffer, recordIndex);
    return output;
}
