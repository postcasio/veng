// Emits a fullscreen triangle from the vertex index alone (no vertex inputs);
// passes through the UV used to sample the covered region. Reads the SPIR-V
// VertexIndex builtin directly rather than SV_VertexID: the latter subtracts
// the base-vertex offset and so pulls in the DrawParameters capability, which
// the device does not enable.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain()
{
    int vertexID = spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); };
    VSOutput output;
    output.v_UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.sv_position = float4(output.v_UV * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
