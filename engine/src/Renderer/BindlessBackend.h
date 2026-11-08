#pragma once

// What the device-free BindlessRegistry asks of the backend compiled in: which frame slot a release
// waits behind, and whether a slot's GPU work has finished. The descriptor writes themselves are the
// registry's private Write* members, which each backend defines.

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng::Renderer::BindlessBackend
{
    /// @brief The frame-in-flight bucket a slot released now must wait behind.
    ///
    /// The recording frame's slot while a frame records; otherwise the slot of the frame submitted
    /// last, which is the latest that can still read the released slot.
    /// @param context The owning context.
    /// @return The frame-in-flight index of the release bucket.
    [[nodiscard]] u32 GetReleaseSlot(const Context& context);

    /// @brief Whether frame slot @p slot's per-frame regions may be written now.
    ///
    /// True for the recording frame's own slot, and for any slot whose last submitted frame has
    /// completed.
    /// @param context The owning context.
    /// @param slot    The frame-in-flight index.
    /// @return Whether no GPU work still reads the slot's regions.
    [[nodiscard]] bool IsSlotWritable(const Context& context, u32 slot);
}
