// Samples an RGBA16F 3D texture at each voxel center and writes the fetched
// value into a 3D storage image. With a nearest-filtered combined sampler the
// voxel-center fetch returns the exact stored texel, so the output volume is an
// identity copy of the input the test can read back and verify. Set 0 is the
// bindless registry; the sampled source (binding 0) and storage destination
// (binding 1) bind on set 1, matching the test's hand-built layout. Drives the
// 3D sampled-image read path (the MoltenVK 3D-storage probe).
[[vk::binding(0, 1)]] Sampler3D u_Source;
[[vk::binding(1, 1)]] [format("rgba16f")] RWTexture3D<float4> u_Destination;

struct VolumePush
{
    uint3 Extent;
};
[[vk::push_constant]] VolumePush u_Push;

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
    u_Destination[id] = u_Source.SampleLevel(uvw, 0.0);
}
