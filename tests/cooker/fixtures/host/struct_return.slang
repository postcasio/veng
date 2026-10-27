// A struct's emitted C++ name is mangled, so compile-host refuses one crossing the boundary.
struct Span
{
    float Low;
    float High;
};

export __extern_cpp Span MakeSpan(float x)
{
    Span span;
    span.Low = x;
    span.High = x;
    return span;
}
