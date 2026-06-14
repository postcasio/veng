#pragma once

// GPU-band test support: probe for a usable Vulkan driver
// so a GPU test can report *skipped* rather than *failed* on a machine with no
// ICD. Consumed by the GPU tests; the pure-logic and type-mapping bands
// never call it.
//
// This header pulls in no Vulkan headers — the probe lives in GpuProbe.cpp,
// which links Vulkan PRIVATE — so consumers stay free of the backend include
// graph (same discipline as the public/backend split).

namespace Veng::Test
{
    // True if a minimal Vulkan instance can be created (i.e. a loader + ICD is
    // present and usable). Creates and immediately destroys a throwaway
    // instance; cheap enough to call once at the top of a GPU test's main().
    [[nodiscard]] bool HasVulkanDriver();
}
