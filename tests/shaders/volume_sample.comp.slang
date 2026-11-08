// Samples an RGBA16F 3D texture at each voxel center and writes the fetched
// value into a 3D storage image. With a nearest-filtered combined sampler the
// voxel-center fetch returns the exact stored texel, so the output volume is an
// identity copy of the input the test can read back and verify. Sets 0-2 are the
// bindless registries; the sampled source (binding 0) and storage destination
// (binding 1) bind on set 3, matching the test's hand-built layout. Drives the
// 3D sampled-image read path (the MoltenVK 3D-storage probe).

#include "Veng/binding.slang"
struct VolumeSampleSet3
{
    Sampler3D Source;
    [format("rgba16f")] RWTexture3D<float4> Destination;
};
VE_DESCRIPTOR_SET(3, VolumeSampleSet3, g_Set3);

struct VolumePush
{
    uint3 Extent;
};
VE_PUSH_CONSTANTS(VolumePush, u_Push);

[shader("compute")]
[numthreads(4, 4, 4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= u_Push.Extent))
    {
        return;
    }
    // Voxel-center UVW: (id + 0.5) / extent maps to the texel's center.
    float3 uvw = (float3(id) + 0.5) / float3(u_Push.Extent);
    g_Set3.Destination[id] = g_Set3.Source.SampleLevel(uvw, 0.0);
}
