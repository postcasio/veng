// Writes each indirect command's instanceCount (1 = survivor, 0 = culled) into a
// fixed-max VkDrawIndexedIndirectCommand buffer, the compute->indirect handoff the
// GPU cull drives. Set 0 is reserved for the bindless registry, so the command
// buffer binds on set 1. Each command is 5 u32s (indexCount, instanceCount,
// firstIndex, vertexOffset, firstInstance) = 20 bytes; instanceCount is word 1.
[[vk::binding(0, 1)]] RWStructuredBuffer<uint> u_Commands;

static const uint WordsPerCommand = 5;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    // Command 0 survives (instanceCount = 1), command 1 is culled (0).
    u_Commands[0 * WordsPerCommand + 1] = 1;
    u_Commands[1 * WordsPerCommand + 1] = 0;
}
