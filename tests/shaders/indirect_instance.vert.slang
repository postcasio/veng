// Emits a fullscreen triangle from the vertex index and passes through the per-instance candidate
// id, read from the instance-rate attribute after the per-vertex one. An indirect draw carries the
// candidate in the command's firstInstance, so the instance-rate fetch lands on the indexed element
// — the candidate-index read mechanism the GPU cull depends on.
struct VSInput
{
    [[vk::location(0)]] float a_Unused : POSITION0;
    [[vk::location(1)]] uint a_CandidateId : CANDIDATE0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    nointerpolation uint v_CandidateId : CANDIDATE0;
};

[shader("vertex")]
VSOutput vsMain(VSInput input, uint vertexIndex : SV_VulkanVertexID)
{
    int vertexID = int(vertexIndex);
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);

    VSOutput output;
    output.sv_position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.v_CandidateId = input.a_CandidateId;
    return output;
}
