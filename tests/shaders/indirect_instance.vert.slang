// Emits a fullscreen triangle from the vertex index alone (no per-vertex buffer)
// and passes through a per-instance color read from an instance-rate vertex
// attribute at location 0. An indirect draw carries the candidate index in the
// command's firstInstance, so the instance-rate fetch lands on the indexed
// element — the candidate-index read mechanism the GPU cull depends on. Reads the
// VertexIndex builtin directly rather than SV_VertexID, which would pull in the
// DrawParameters capability the device does not enable.
struct VSInput
{
    float4 a_Color : COLOR0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    int vertexID = spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); };
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);

    VSOutput output;
    output.sv_position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.v_Color = input.a_Color;
    return output;
}
