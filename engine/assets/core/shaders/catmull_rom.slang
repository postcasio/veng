// Catmull-Rom reconstruction of a bilinearly filtered texture, for history reads at a reprojected
// position: a bilinear read at a fractional position blurs, and a history read that way every frame
// accumulates the blur, where the cubic keeps a thin, sharp feature sharp.

// The texture's Catmull-Rom reconstruction at `uv`, from the 4x4 texels around it collapsed into nine
// bilinear fetches (the middle two weights of each axis share one fetch at their weighted centre).
// `extent` is the texture's size in texels. The cubic's negative lobes can overshoot below zero, so
// the result is clamped to it.
float4 SampleCatmullRom(Texture2D source, SamplerState samp, float2 uv, float2 extent)
{
    const float2 texelSize = 1.0 / extent;
    const float2 samplePos = uv * extent;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;

    const float2 f = samplePos - texPos1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / w12;

    const float2 texPos0 = (texPos1 - 1.0) * texelSize;
    const float2 texPos3 = (texPos1 + 2.0) * texelSize;
    const float2 texPos12 = (texPos1 + offset12) * texelSize;

    float4 result = 0.0;
    result += source.SampleLevel(samp, float2(texPos0.x, texPos0.y), 0.0) * w0.x * w0.y;
    result += source.SampleLevel(samp, float2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += source.SampleLevel(samp, float2(texPos3.x, texPos0.y), 0.0) * w3.x * w0.y;

    result += source.SampleLevel(samp, float2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += source.SampleLevel(samp, float2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += source.SampleLevel(samp, float2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;

    result += source.SampleLevel(samp, float2(texPos0.x, texPos3.y), 0.0) * w0.x * w3.y;
    result += source.SampleLevel(samp, float2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    result += source.SampleLevel(samp, float2(texPos3.x, texPos3.y), 0.0) * w3.x * w3.y;

    return max(result, 0.0);
}
