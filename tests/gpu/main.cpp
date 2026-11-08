// doctest entry point for the in-process multi-case GPU integration suite.
//
// Custom main (not DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN): checks
// Test::HasGpuDevice() first and returns 77 if no usable GPU device is
// present, matching the SKIP_RETURN_CODE 77 contract of the rest of the `gpu`
// label band. Otherwise hands off to doctest as usual.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <support/GpuProbe.h>

int main(int argc, char** argv)
{
    if (!Veng::Test::HasGpuDevice())
    {
        return 77;
    }

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);

    const int res = ctx.run();
    if (ctx.shouldExit())
    {
        return res;
    }

    return res;
}
