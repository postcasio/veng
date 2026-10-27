// The host instantiations of gradient_noise.slang: one exported wrapper per type the host
// evaluates. GPU shaders never include this file.
#include "gradient_noise.slang"

/// @brief Gradient noise in float: value in .x, gradient in .yzw.
export __extern_cpp float4 NoiseF32(float3 p)
{
    return GradientNoise<float>(p);
}

/// @brief Gradient noise in double: value in .x, gradient in .yzw.
export __extern_cpp double4 NoiseF64(double3 p)
{
    return GradientNoise<double>(p);
}

/// @brief The fractal sum in float: value in .x, gradient in .yzw.
export __extern_cpp float4 FbmF32(float3 p, uint octaves)
{
    return GradientNoiseFbm<float>(p, octaves);
}

/// @brief The fractal sum in double: value in .x, gradient in .yzw.
export __extern_cpp double4 FbmF64(double3 p, uint octaves)
{
    return GradientNoiseFbm<double>(p, octaves);
}
