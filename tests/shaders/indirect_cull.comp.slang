// Writes each indirect command's instanceCount (1 = survivor, 0 = culled) into a
// fixed-max VkDrawIndexedIndirectCommand buffer, the compute->indirect handoff the
// GPU cull drives. Sets 0-2 are reserved for the bindless registries, so the command
// buffer binds on set 3. Each command is 5 u32s (indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance) = 20 bytes; instanceCount is word 1.

#include "Veng/binding.slang"
struct IndirectCullSet3
{
    RWStructuredBuffer<uint> Commands;
};
VE_DESCRIPTOR_SET(3, IndirectCullSet3, g_Set3);

static const uint WordsPerCommand = 5;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    // Command 0 survives (instanceCount = 1), command 1 is culled (0).
    g_Set3.Commands[0 * WordsPerCommand + 1] = 1;
    g_Set3.Commands[1 * WordsPerCommand + 1] = 0;
}
