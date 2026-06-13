#pragma once

// Per-case GPU fixture (planset-4, plan 05): wraps Test::GpuContext so each
// TEST_CASE_FIXTURE instance gets its own Context::Initialize/Dispose
// lifecycle (doctest default-constructs the fixture per test case and
// destroys it after), giving every case a fresh device — the per-case
// isolation the old Context singleton made impossible.
//
// The extent here is the *internal render extent*; it has nothing to do with
// per-resource extents (Buffer sizes, Image extents), which each case sets
// explicitly, as the migrated one-exe tests already did.

#include <support/GpuContext.h>

namespace Veng::Test
{
    struct GpuFixture
    {
        GpuContext Gpu{"veng_gpu", {64, 64}};
        Renderer::Context& Context = Gpu.Get();
    };
}
