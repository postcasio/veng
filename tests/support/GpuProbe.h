#pragma once

// GPU-band test support: probes for a device the engine's backend can use, so a GPU test can
// report *skipped* rather than *failed* on a machine with none. Consumed by the GPU tests; the
// pure-logic bands never call it. Both answers are the backend's own (Veng/Renderer/DeviceProbe.h),
// so this header names no graphics API.

namespace Veng::Test
{
    // True if the backend compiled in finds a usable device. Creates and immediately destroys
    // whatever it probes with; cheap enough to call once at the top of a GPU test's main().
    [[nodiscard]] bool HasGpuDevice();

    // True if a usable device samples ASTC LDR textures. Gates a test whose cooked assets are
    // ASTC-compressed: a device without the codec gets AssetError::Unsupported and samples the
    // fallback, so such a test must skip rather than fail. False when no usable device is present.
    [[nodiscard]] bool HasAstcSupport();
}
