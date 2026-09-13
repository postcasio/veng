#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

/// @brief The seam a consumer hands the compositor an image to composite the presented frame into.
///
/// The frame the window shows is produced by one fullscreen composite of the gathered viewports
/// and the overlay; a sink asks for that composite to be run a **second** time, into an image the
/// consumer owns, with a colour space of the consumer's choosing. It is the zero-copy alternative
/// to reading the finished frame back: nothing is copied out of the swap chain, the encoding is a
/// setting rather than a consequence of the display, and the presented frame is untouched.
namespace Veng::Renderer
{
    class Image;

    /// @brief One frame's capture target: where to composite, how to encode it, and what to include.
    struct CaptureTarget
    {
        /// @brief The image the capture composite renders into.
        ///
        /// Its extent must equal the presented extent the sink was asked with, and its format must
        /// be one the composite can write.
        Ref<Image> Image;

        /// @brief The colour space the capture composite encodes for.
        ///
        /// Independent of the display's: SrgbNonlinear writes linear values into an _SRGB-format
        /// target whose store encodes them, Hdr10St2084 converts primaries and PQ-encodes.
        DisplayColorSpace ColorSpace = DisplayColorSpace::SrgbNonlinear;

        /// @brief Whether the application's overlay is composited over the gathered viewports.
        ///
        /// The overlay is the application's own chrome — a debug shell, an editor's panels — while
        /// a game's HUD and menus are driven into the viewports and so are part of the gathered
        /// image either way. Off, the capture is the gathered viewports alone.
        bool IncludeOverlay = false;
    };

    /// @brief A consumer of the composited frame, asked for a target once per frame.
    ///
    /// Implemented by whatever owns the images being composited into — a video recorder's buffer
    /// pool, a streaming encoder. Installed on the compositor with
    /// ViewportCompositor::SetCaptureSink; the compositor never owns the sink.
    class CaptureSink
    {
    public:
        /// @brief Destroys the sink.
        virtual ~CaptureSink() = default;

        /// @brief Returns the target the capture composite renders into for this frame's slot.
        ///
        /// Called once per composited frame, before the capture composite records. Returning
        /// nullopt skips the capture composite for that frame, which is how a sink idles or
        /// declines a frame it has no image for.
        /// @param slot            The frame-in-flight slot this frame records into.
        /// @param presentedExtent The extent the presented frame is composited at.
        /// @return The target to composite into, or nullopt to skip this frame.
        /// @post A returned target's image extent equals @p presentedExtent.
        virtual optional<CaptureTarget> AcquireTarget(u32 slot, uvec2 presentedExtent) = 0;

        /// @brief Announces that the frame recorded into @p slot has had its fence waited.
        ///
        /// Everything that frame submitted is complete, so the target it was given is readable.
        /// Fires for every slot the compositor's context retires while a sink is installed,
        /// including slots the sink declined a target for.
        /// @param slot The frame-in-flight slot that retired.
        virtual void OnSlotRetired(u32 slot) = 0;
    };
}
