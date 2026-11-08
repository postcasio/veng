// Emits a fullscreen triangle from the vertex index alone (no vertex inputs);
// passes through the UV used to sample the covered region. SV_VulkanVertexID is
// the raw vertex index: SV_VertexID would subtract the base-vertex offset and so
// pull in the DrawParameters capability, which the device does not enable.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain(uint vertexIndex : SV_VulkanVertexID)
{
    int vertexID = int(vertexIndex);
    VSOutput output;
    output.v_UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.sv_position = float4(output.v_UV * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
