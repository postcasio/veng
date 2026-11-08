// Culls eight candidates, of which every one whose index is not a multiple of three survives, into
// indirect records at a 32-byte stride. Mode 0 writes every candidate's record in its own slot, a
// culled one with no instances (the fixed-maximum path); mode 1 appends each survivor's indexed
// record to the front of the buffer and counts it, and mode 2 does the same with non-indexed
// records (the count-driven path). Every record draws one instance whose base is its candidate.
#include "Veng/binding.slang"

struct IndirectCountCullSet3
{
    RWStructuredBuffer<uint> Records;
    RWStructuredBuffer<uint> Count;
};
VE_DESCRIPTOR_SET(3, IndirectCountCullSet3, g_Set3);

struct IndirectCountCullPush
{
    uint Mode;
};
VE_PUSH_CONSTANTS(IndirectCountCullPush, g_PC);

static const uint WordsPerRecord = 8;
static const uint Candidates = 8;

[shader("compute")]
[numthreads(8, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    const uint candidate = id.x;
    if (candidate >= Candidates)
    {
        return;
    }
    const bool survives = candidate % 3u != 0u;
    if (g_PC.Mode == 0u)
    {
        const uint at = candidate * WordsPerRecord;
        g_Set3.Records[at + 0] = 3u;
        g_Set3.Records[at + 1] = survives ? 1u : 0u;
        g_Set3.Records[at + 2] = 0u;
        g_Set3.Records[at + 3] = 0u;
        g_Set3.Records[at + 4] = candidate;
        return;
    }
    if (!survives)
    {
        return;
    }
    uint slot = 0u;
    InterlockedAdd(g_Set3.Count[0], 1u, slot);
    const uint at = slot * WordsPerRecord;
    if (g_PC.Mode == 1u)
    {
        g_Set3.Records[at + 0] = 3u;
        g_Set3.Records[at + 1] = 1u;
        g_Set3.Records[at + 2] = 0u;
        g_Set3.Records[at + 3] = 0u;
        g_Set3.Records[at + 4] = candidate;
    }
    else
    {
        g_Set3.Records[at + 0] = 3u;
        g_Set3.Records[at + 1] = 1u;
        g_Set3.Records[at + 2] = 0u;
        g_Set3.Records[at + 3] = candidate;
    }
}
