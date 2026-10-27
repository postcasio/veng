// A struct and a fixed-size array across the host boundary, for the generated header's shape.
struct Span
{
    float Low;
    float High;
};

export __extern_cpp Span Widen(Span span, float4 values[4], out float sums[4])
{
    for (uint i = 0; i < 4; ++i)
    {
        sums[i] = values[i].x + values[i].y + values[i].z + values[i].w;
        span.Low = min(span.Low, sums[i]);
        span.High = max(span.High, sums[i]);
    }
    return span;
}
