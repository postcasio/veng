#include "Veng/instanced_mesh_math.slang"

// Depth-only vertex stage for an instanced mesh set's shadow casters. VSInput is veng's canonical
// vertex layout on binding 0 with no instance-rate binding: the draw's firstInstance is the start of
// a level's survivor list in the frame's instance buffer, so the vertex's instance index
// (SV_VulkanInstanceID, which counts from firstInstance) names its list entry, and the entry names
// the instance's record. The push block carries the shadow view's view-projection already composed
// with the set's frame, so it changes per set and view, not per draw.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
};

struct ShadowInstancedPushConstants
{
    float4x4 ViewProjWorld; // set frame -> light clip
    uint InstanceBuffer;    // bindless storage slot of the frame's instance buffer
    uint InstanceBase;      // record index of the set's first instance
};

[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];

[[vk::push_constant]] ConstantBuffer<ShadowInstancedPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input, uint listIndex : SV_VulkanInstanceID)
{
    const ByteAddressBuffer instances = g_Buffers[g_PC.InstanceBuffer];
    const uint instance = instances.Load(listIndex * 4u);
    const InstanceRecord record =
        instances.Load<InstanceRecord>((g_PC.InstanceBase + instance) * InstanceRecordBytes);

    VSOutput output;
    output.sv_position =
        mul(g_PC.ViewProjWorld, mul(InstanceMatrix(record), float4(input.a_Position, 1.0)));
    return output;
}
