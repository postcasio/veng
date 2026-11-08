// Intrinsics one compilation target lacks, spelled once for both: each calls the intrinsic on
// SPIR-V, so the SPIR-V is what calling it directly produces, and its nearest equivalent
// elsewhere. Each must inline, or the target switch would sit behind a function call.
#pragma once

// The per-pixel screen-space derivative. Metal has no fine/coarse distinction: its dfdx/dfdy
// differentiate within the 2x2 quad per pixel.
[ForceInline]
float DdxFine(float value)
{
    __target_switch
    {
    case spirv:
        return ddx_fine(value);
    default:
        return ddx(value);
    }
}

[ForceInline]
float3 DdxFine(float3 value)
{
    __target_switch
    {
    case spirv:
        return ddx_fine(value);
    default:
        return ddx(value);
    }
}

[ForceInline]
float DdyFine(float value)
{
    __target_switch
    {
    case spirv:
        return ddy_fine(value);
    default:
        return ddy(value);
    }
}

[ForceInline]
float3 DdyFine(float3 value)
{
    __target_switch
    {
    case spirv:
        return ddy_fine(value);
    default:
        return ddy(value);
    }
}

// The number of lanes below this one, among the active lanes, for which the predicate holds.
// Metal has no prefix bit count, so it takes the exclusive prefix sum of the predicate.
[ForceInline]
uint WavePrefixCount(bool predicate)
{
    __target_switch
    {
    case spirv:
        return WavePrefixCountBits(predicate);
    default:
        return WavePrefixSum(predicate ? 1u : 0u);
    }
}
