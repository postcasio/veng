#pragma once

// GPU-band test support: shared bring-up/teardown for GPU tests, plus a
// pixel-compare helper for the downloaded-RGBA8 pattern common to those tests.
//
// This header pulls in only public veng APIs (Context, Buffer, ...) — no
// vk:: / VMA / GLFW types — so it stays clear of the public/backend split the
// include_hygiene test guards.
//
// Each one-exe GPU test owns its own process and its own Context; the
// in-process veng_gpu suite instead constructs one of these per
// TEST_CASE_FIXTURE for per-case isolation.

#include <array>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Context.h>

namespace Veng::Test
{
    // RAII headless Context: Initialize()s in the constructor (off-screen, no
    // window) and runs the WaitIdle/DisposeResources/Dispose teardown sequence
    // in the destructor. Construct this *after* checking HasVulkanDriver() —
    // it has no skip path of its own.
    //
    // Context can't be copied or moved (it owns a Unique<Native>), so this
    // wrapper holds it by value and is itself non-movable/non-copyable.
    class GpuContext
    {
    public:
        // extent is the internal render extent; most of these tests render or
        // clear an off-screen image at this size. 4x4 matches the existing
        // smoke tests' default.
        explicit GpuContext(string_view applicationName, uvec2 extent = {4, 4})
        {
            m_Context.Initialize(
                {
                    .ApplicationName = string(applicationName),
                    .InternalRenderExtent = extent,
                },
                nullptr);
        }

        ~GpuContext()
        {
            m_Context.WaitIdle();
            m_Context.DisposeResources();
            m_Context.Dispose();
        }

        GpuContext(const GpuContext&) = delete;
        GpuContext& operator=(const GpuContext&) = delete;
        GpuContext(GpuContext&&) = delete;
        GpuContext& operator=(GpuContext&&) = delete;

        [[nodiscard]] Renderer::Context& Get() { return m_Context; }

    private:
        Renderer::Context m_Context;
    };

    // Compares `pixels` (an RGBA8 buffer, as returned by Image::Download()) to
    // `expected`, a single 4-byte RGBA8 value repeated across every pixel.
    // Prints a "FAIL: ..." line to stderr naming the first mismatching pixel and
    // channel. Returns true iff every pixel matches (and the size is a non-zero
    // multiple of 4 bytes).
    [[nodiscard]] bool PixelsMatch(std::span<const u8> pixels, const std::array<u8, 4>& expected);
}
