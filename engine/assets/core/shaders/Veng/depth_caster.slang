// The per-mesh record an instanced depth pass reads (GpuCasterRecord on the C++ side): one per
// visible mesh, written once a frame by the renderer and shared by every shadow view and the
// depth+normal prepass. A depth vertex stage indexes it with its instance-rate a_CandidateId
// attribute, which each draw's firstInstance selects from the pass's instance-id buffer, so one
// instanced draw places every instance of a submesh at its own transform.
struct CasterRecord
{
    float4x4 World;        // object -> world
    float4 NormalColumn0;  // world normal matrix (inverse-transpose of World's 3x3), column 0 (xyz)
    float4 NormalColumn1;  // column 1
    float4 NormalColumn2;  // column 2
};

// Set 3 is the first author set; sets 0-2 are the bindless registry's.
[[vk::binding(0, 3)]] StructuredBuffer<CasterRecord> g_Casters;
