// doctest entry point for the in-process multi-case GPU integration suite
// (planset-4, plan 05).
//
// Custom main (not DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN): checks
// Test::HasVulkanDriver() first and returns 77 if no usable Vulkan ICD is
// present, matching the SKIP_RETURN_CODE 77 contract of the rest of the `gpu`
// label band. Otherwise hands off to doctest as usual.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <support/GpuProbe.h>

int main(int argc, char** argv)
{
    if (!Veng::Test::HasVulkanDriver())
        return 77;

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);

    const int res = ctx.run();
    if (ctx.shouldExit())
        return res;

    return res;
}
