// Places one triangle from three clip-space positions in the push block, so a test states exactly
// where a primitive lands and which way it winds. The z of each position is its depth.
#include "Veng/binding.slang"

struct RasterProbePush
{
    float4 Positions[3];
    float4 Color;
};
VE_PUSH_CONSTANTS(RasterProbePush, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
};

[shader("vertex")]
VSOutput vsMain(uint vertexIndex : SV_VulkanVertexID)
{
    VSOutput output;
    output.sv_position = float4(g_PC.Positions[vertexIndex].xyz, 1.0);
    return output;
}
