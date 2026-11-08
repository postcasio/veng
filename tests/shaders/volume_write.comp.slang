// Writes a known field into an RGBA16F 3D storage image: each voxel stores its
// own integer coordinate in RGB and a constant in A. Sets 0-2 are reserved for the
// bindless registries, so the storage image binds on set 3; the test's hand-built
// layout matches this binding. Drives the 3D storage-image write path (the
// MoltenVK 3D-storage probe).

#include "Veng/binding.slang"
struct VolumeWriteSet3
{
    [format("rgba16f")] RWTexture3D<float4> Volume;
};
VE_DESCRIPTOR_SET(3, VolumeWriteSet3, g_Set3);

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
    g_Set3.Volume[id] = float4(float(id.x), float(id.y), float(id.z), 1.0);
}
