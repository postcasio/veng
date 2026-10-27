// Slang lowers an array return to a trailing out pointer, so compile-host refuses one; an out
// parameter carries it instead.
export __extern_cpp float2[2] Pair(float x)
{
    float2 pair[2];
    pair[0] = float2(x);
    pair[1] = float2(-x);
    return pair;
}
