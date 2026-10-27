// The host module the SDK conformance consumer compiles: it includes an engine header to prove
// the engine core shader dir is on the search path, and exports one function.
#include "Veng/sh.slang"

export __extern_cpp float3 ScaleHalf(float3 v)
{
    return v * 0.5;
}
