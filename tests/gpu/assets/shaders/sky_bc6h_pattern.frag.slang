// A sky whose radiance is a known pattern of the texel it lands on, for checking a block-compressed
// bake against a float one: a smooth high-dynamic-range hue gradient (mode 0), a dark
// field with at most one bright texel per 4x4 block (mode 1), or independent per-texel speckle over
// two decades of luminance with a modest spread of chroma (mode 2).
#include "Veng/sky.slang"

struct MaterialParams
{
    float4 Mode; // x the pattern, y the face size in texels
};

VE_PUSH_CONSTANTS(SkyPushConstants, g_SkyPC);

uint Hash(uint x)
{
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Unit(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// The cube face a direction falls in, in the bake's layer order (+X, -X, +Y, -Y, +Z, -Z).
uint FaceOf(float3 d)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        return d.x > 0.0 ? 0u : 1u;
    }
    if (a.y >= a.z)
    {
        return d.y > 0.0 ? 2u : 3u;
    }
    return d.z > 0.0 ? 4u : 5u;
}

[shader("fragment")]
float4 fsMain(SkyFragmentInput input) : SV_Target0
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(g_SkyPC.MaterialOffset);
    const float3 dir = SkyViewDirection(g_SkyPC, input.v_UV);
    if (p.Mode.x < 0.5)
    {
        // Six octaves of red against four of blue, running opposite ways along one axis.
        return float4(exp2(3.0 * dir.x), 0.5 * exp2(dir.x), exp2(-2.0 * dir.x), 1.0);
    }

    const uint faceSize = uint(p.Mode.y);
    const uint2 texel = min(uint2(input.v_UV * float(faceSize)), uint2(faceSize - 1));
    const uint face = FaceOf(dir);
    const uint id = (face * faceSize + texel.y) * faceSize + texel.x;
    if (p.Mode.x < 1.5)
    {
        const uint2 block = texel / 4;
        const uint blockId = (face * faceSize + block.y) * faceSize + block.x;
        const uint h = Hash(blockId);
        const uint2 point = uint2(Hash(h) & 3u, (Hash(h) >> 2) & 3u);
        if (Unit(h) < 0.35 && all(texel % 4 == point))
        {
            return float4(float3(40.0, 25.0, 12.0) * (0.5 + Unit(Hash(h + 1u))), 1.0);
        }
        return float4(0.002 * (1.0 + 0.5 * dir), 1.0);
    }

    const float luminance = 0.05 * pow(100.0, Unit(Hash(id)));
    const float3 chroma = float3(1.0 + 0.2 * (Unit(Hash(id + 1u)) - 0.5), 1.0,
                                 1.0 + 0.2 * (Unit(Hash(id + 2u)) - 0.5));
    return float4(luminance * chroma, 1.0);
}
