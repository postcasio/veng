#pragma once

// Per-case GPU fixture: wraps Test::GpuContext so each TEST_CASE_FIXTURE
// instance gets its own Context::Initialize/Dispose lifecycle (doctest
// default-constructs the fixture per test case and destroys it after),
// giving every case a fresh device.
//
// The extent here is the *internal render extent*; it has nothing to do with
// per-resource extents (Buffer sizes, Image extents), which each case sets
// explicitly.

#include <support/GpuContext.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::Test
{
    struct GpuFixture
    {
        GpuContext Gpu{"veng_gpu", {64, 64}};
        Renderer::Context& Context = Gpu.Get();

        // A task system AssetManager takes by reference. The loader cases here
        // use LoadSync (the blocking UploadSync path, no transfer pool), so the
        // pools are not wired up; a case exercising async Load wires its own.
        TaskSystem Tasks{TaskSystemInfo{.WorkerCount = 2}};

        // A type registry AssetManager takes by reference (the prefab loader
        // reflects through it). Empty here — these cases load non-prefab assets.
        TypeRegistry Types;
    };
}
