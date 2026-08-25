// Writes a normalised range into a 16-bit-per-channel unorm storage image, one texel per
// invocation: R the floor of the range, G its ceiling, B the datum halfway between, A a
// ramp across x. Sets 0-2 are the bindless registries, so the storage image binds on set
// 3; the test's hand-built layout matches this binding. [format("rgba16")] compiles to a
// SPIR-V Rgba16 image, one of the extended storage-image formats — so this shader is also
// the probe that the device's shaderStorageImageExtendedFormats support is real.
[[vk::binding(0, 3)]] [format("rgba16")] RWTexture2D<float4> u_Target;

struct RangePush
{
    uint2 Extent;
};
[[vk::push_constant]] RangePush u_Push;

[shader("compute")]
[numthreads(8, 4, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= u_Push.Extent))
    {
        return;
    }
    float ramp = float(id.x) / float(max(u_Push.Extent.x - 1u, 1u));
    u_Target[id.xy] = float4(0.0, 1.0, 0.5, ramp);
}
