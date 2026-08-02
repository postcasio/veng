// Shared grid vocabulary for the stable-fluids kernels: the per-axis wrap fold, the manual
// bilinear tap the semi-Lagrangian advection reads, and the per-row metric that stretches the
// x axis. Every kernel binds its own resources on set 1 (a closed producer->consumer chain off
// the bindless registry) and takes this one push block.
#ifndef VENG_FLUID_COMMON
#define VENG_FLUID_COMMON

static const uint FluidWrapPeriodic = 0u;
static const uint FluidWrapClamped = 1u;

// Jacobi iteration 0 reads no pressure: the solve starts from zero every step.
static const uint FluidFlagFirstIteration = 1u;
static const uint FluidFlagHasTarget = 2u;
static const uint FluidFlagHasDamping = 4u;

struct FluidParams
{
    uint Width;
    uint Height;
    uint WrapX;
    uint WrapY;
    float TimeStep;
    float Scale;      // advection survival, confinement strength, or store scale, per kernel
    float Relaxation; // relaxation rate toward the target field
    uint Flags;
};

[[vk::push_constant]] ConstantBuffer<FluidParams> g_Fluid;

bool FluidOutside(uint3 id)
{
    return id.x >= g_Fluid.Width || id.y >= g_Fluid.Height;
}

int FluidFoldAxis(int c, int n, uint wrap)
{
    if (wrap == FluidWrapPeriodic)
    {
        int folded = c % n;
        return folded < 0 ? folded + n : folded;
    }
    return clamp(c, 0, n - 1);
}

int2 FluidFold(int2 c)
{
    return int2(FluidFoldAxis(c.x, int(g_Fluid.Width), g_Fluid.WrapX),
                FluidFoldAxis(c.y, int(g_Fluid.Height), g_Fluid.WrapY));
}

float4 FluidLoad(Texture2D<float4> field, int2 c)
{
    return field.Load(int3(FluidFold(c), 0));
}

// The bilinear tap is filtered by hand rather than through a sampler: a sampler carries one
// address mode per axis and cannot express the same fold the projection assumes at a clamped
// wall, and hand filtering is what makes the CPU reference tap comparable texel for texel.
float4 FluidSampleBilinear(Texture2D<float4> field, float2 position)
{
    float2 offset = position - 0.5;
    float2 origin = floor(offset);
    float2 fraction = offset - origin;
    int2 base = int2(origin);

    float4 s00 = FluidLoad(field, base + int2(0, 0));
    float4 s10 = FluidLoad(field, base + int2(1, 0));
    float4 s01 = FluidLoad(field, base + int2(0, 1));
    float4 s11 = FluidLoad(field, base + int2(1, 1));
    return lerp(lerp(s00, s10, fraction.x), lerp(s01, s11, fraction.x), fraction.y);
}

// A row of metric m advances m grid cells per unit of x velocity and has its x derivatives
// scaled by m — a stretch on the grid, nothing more.
float FluidRowMetric(StructuredBuffer<float> metric, int row)
{
    return metric[FluidFoldAxis(row, int(g_Fluid.Height), g_Fluid.WrapY)];
}

#endif
