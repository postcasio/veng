// A host module that includes a helper, with an out parameter beside a return value.
#include "scale_helper.slang"

export __extern_cpp float2 ScalePair(float2 v, out float sum)
{
    sum = v.x + v.y;
    return float2(ScaleValue(v.x), ScaleValue(v.y));
}
