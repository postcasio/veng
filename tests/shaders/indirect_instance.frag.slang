// Writes the candidate id the vertex stage read at the command's base instance into red, so the
// downloaded pixel names the candidate that drew.
struct VSOutput
{
    float4 sv_position : SV_Position;
    nointerpolation uint v_CandidateId : CANDIDATE0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return float4(float(input.v_CandidateId) / 255.0, 0.0, 0.0, 1.0);
}
