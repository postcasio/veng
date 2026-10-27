// Exports taking and returning structs, fixed-size arrays and matrices, compiled for the host. The
// arithmetic stays on small integers, so the host build is held to its references exactly.

struct Span
{
    float Low;
    float High;
};

struct Block
{
    float4 Values[4];
    Span Range;
    float3x2 Basis; // 3 rows, 2 columns
    uint Count;
};

/// @brief The first Count values weighted and summed, then clamped to the block's range.
export __extern_cpp float4 SumBlock(Block block, float weights[4])
{
    float4 sum = float4(0.0);
    for (uint i = 0; i < block.Count; ++i)
    {
        sum += block.Values[i] * weights[i];
    }
    return clamp(sum, float4(block.Range.Low), float4(block.Range.High));
}

/// @brief The block's basis applied to a column vector.
export __extern_cpp float3 ApplyBasis(Block block, float2 v)
{
    return mul(block.Basis, v);
}

/// @brief Each value's component sum, and the range widened to cover every one.
export __extern_cpp void Summarise(Block block, out float sums[4], inout Span range)
{
    for (uint i = 0; i < 4; ++i)
    {
        const float4 v = block.Values[i];
        sums[i] = v.x + v.y + v.z + v.w;
        range.Low = min(range.Low, sums[i]);
        range.High = max(range.High, sums[i]);
    }
}

/// @brief The span between two values, ordered.
export __extern_cpp Span MakeSpan(float a, float b)
{
    Span span;
    span.Low = min(a, b);
    span.High = max(a, b);
    return span;
}

/// @brief A matrix passed and returned on its own: the transpose, scaled.
export __extern_cpp float2x3 ScaledTranspose(float3x2 m, float scale)
{
    return transpose(m) * scale;
}

/// @brief The summed width of an array of spans.
export __extern_cpp float TotalWidth(Span spans[3])
{
    float total = 0.0;
    for (uint i = 0; i < 3; ++i)
    {
        total += spans[i].High - spans[i].Low;
    }
    return total;
}
