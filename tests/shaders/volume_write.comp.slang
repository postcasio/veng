// Writes a known field into an RGBA16F 3D storage image: each voxel stores its
// own integer coordinate in RGB and a constant in A. Set 0 is reserved for the
// bindless registry, so the storage image binds on set 1; the test's hand-built
// layout matches this binding. Drives the 3D storage-image write path (the
// MoltenVK 3D-storage probe).
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture3D<float4> u_Volume;

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
    u_Volume[id] = float4(float(id.x), float(id.y), float(id.z), 1.0);
}
