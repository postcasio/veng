// Outputs the per-instance color passed through from the vertex stage — the
// instance-rate attribute the indirect command's firstInstance selected. The
// downloaded pixel proving the right color reached the shader confirms the base
// instance reached the GPU.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return input.v_Color;
}
