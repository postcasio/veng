// Writes the push block's colour.
#include "Veng/binding.slang"

struct RasterProbePush
{
    float4 Positions[3];
    float4 Color;
};
VE_PUSH_CONSTANTS(RasterProbePush, g_PC);

[shader("fragment")]
float4 fsMain(float4 position : SV_Position) : SV_Target
{
    return g_PC.Color;
}
