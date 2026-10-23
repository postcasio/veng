// The bright pass's tap conditioning, binding-free: the bloom downsample and the anamorphic streak's
// prefilter treat a scene tap the same way, so a source seeds both with the same light.
#pragma once

// Rec. 709 relative luminance.
float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// True where every component is finite. Tested on the exponent bits rather than through
// isnan/isinf, which a fast-math shader compile is free to fold away.
bool IsFinite(float3 c)
{
    const uint3 exponent = asuint(c) & 0x7F800000u;
    return all(exponent != 0x7F800000u);
}

// One tap, conditioned before it is averaged: a non-finite tap contributes nothing, and a tap
// brighter than half the ceiling is rolled off toward the ceiling along a smooth shoulder, scaled as
// a whole so its hue is kept. Every tap at or under half the ceiling passes unchanged, so the average
// of ordinary content stays linear — a small source seeds the same light wherever it falls on the
// grid — while no single tap, however extreme, carries more than the ceiling.
float3 ConditionBrightTap(float3 c, float ceiling)
{
    if (!IsFinite(c))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float knee = 0.5 * max(ceiling, 1e-4);
    const float luma = Luma(c);
    if (luma <= knee)
    {
        return c;
    }
    // The shoulder has unit slope at the knee and approaches knee + knee, the ceiling.
    const float over = luma - knee;
    const float limited = knee + over / (1.0 + over / knee);
    return c * (limited / luma);
}

// The soft-knee bright pass's gain for an averaged colour: a smooth quadratic ramp over a
// threshold-wide band above the threshold, so a surface near it fades in rather than popping. A zero
// threshold passes everything through.
float BrightPassGain(float3 average, float threshold)
{
    if (threshold <= 0.0)
    {
        return 1.0;
    }
    const float soft = saturate((Luma(average) - threshold) / threshold);
    return soft * soft;
}
