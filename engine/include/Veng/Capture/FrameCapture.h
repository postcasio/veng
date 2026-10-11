#pragma once

#include <Veng/Veng.h>
#include <Veng/Capture/Screenshot.h>
#include <Veng/Renderer/CaptureSink.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ViewportCompositor;
}

namespace Veng::Capture
{
    /// @brief What one frame capture composites.
    struct FrameCaptureRequest
    {
        /// @brief Whether the application's own overlay is composited over the presented viewports.
        ///
        /// The overlay is the application's chrome — a debug shell, an editor's panels. A world's HUD
        /// and menus are driven into the viewports, so they are in the capture either way; off, the
        /// capture is the frame as a player sees it with the chrome left out.
        bool IncludeOverlay = true;
    };

    /// @brief Captures one composited frame on demand, with or without the application's overlay.
    ///
    /// A one-shot Renderer::CaptureSink: a Request installs it on the compositor, the next composited
    /// frame runs a further composite into an image it owns — in sRGB whatever the display's colour
    /// space, so an HDR display still yields an ordinary picture — and it uninstalls itself once that
    /// frame retires. Nothing is copied out of the swap chain, which the presentation engine owns once
    /// presented, and no frame that did not ask pays anything.
    ///
    /// The capture is readable once its frame has ended (IsPending false) and readable without a
    /// stall once that frame's fence has been waited (IsComplete true); Take reads it either way.
    ///
    /// @warning Available only on a windowed run with a composite tail (an ImGui overlay): a headless
    ///          run composites no frame. IsAvailable() reports it and Request refuses with a reason.
    class FrameCapture final : public Renderer::CaptureSink
    {
    public:
        /// @brief Constructs the capture over the context and the compositor it installs itself on.
        /// @param context     The render context; must outlive the capture.
        /// @param compositor  The compositor the capture installs itself on while a request is open.
        FrameCapture(Renderer::Context& context, Renderer::ViewportCompositor& compositor);

        /// @brief Uninstalls the capture if a request is still open.
        ~FrameCapture() override;

        FrameCapture(const FrameCapture&) = delete;
        FrameCapture& operator=(const FrameCapture&) = delete;

        /// @brief Whether this run composites frames at all: windowed, with a composite tail.
        [[nodiscard]] bool IsAvailable() const;

        /// @brief Asks the next composited frame to be captured, or refuses and says why.
        ///
        /// Refused when the run composites no frame or a capture is already open — requested and not
        /// yet taken or cancelled.
        /// @param request  What the capture composites.
        /// @return Success, or the reason it was refused.
        VoidResult Request(const FrameCaptureRequest& request);

        /// @brief Whether a capture is open but its frame has not ended, so Take cannot read it yet.
        ///
        /// A window with no presentable size composites nothing, so a request made while minimized
        /// stays pending until a frame is composited or the request is cancelled.
        [[nodiscard]] bool IsPending() const;

        /// @brief Whether the captured frame's GPU work has finished, so Take reads it without a stall.
        [[nodiscard]] bool IsComplete() const;

        /// @brief Whether a capture is open: requested and not yet taken or cancelled.
        [[nodiscard]] bool IsOpen() const;

        /// @brief Reads the captured frame and closes the capture.
        ///
        /// Blocks on the download, which waits the captured frame's GPU work when it has not yet
        /// finished. Refused — leaving the capture open — when nothing is open or its frame has not
        /// ended.
        /// @return The captured picture, or why there is none to read.
        Result<RgbImage> Take();

        /// @brief Closes an open capture without reading it. A no-op when none is open.
        void Cancel();

        /// @brief Returns the image this frame's capture composite renders into, when one is wanted.
        ///
        /// Hands out a target only on the first composited frame after a Request; every other frame
        /// is declined.
        /// @param slot            The frame-in-flight slot this frame records into.
        /// @param presentedExtent The extent the presented frame is composited at.
        /// @return The target to composite into, or nullopt.
        optional<Renderer::CaptureTarget> AcquireTarget(u32 slot, uvec2 presentedExtent) override;

        /// @brief Marks the capture complete when the slot it was composited in retires.
        /// @param slot  The frame-in-flight slot that retired.
        void OnSlotRetired(u32 slot) override;

    private:
        /// @brief Where the open capture, if any, has got to.
        enum class Stage : u8
        {
            /// @brief No capture is open.
            Idle,
            /// @brief Requested; waiting for the next composited frame.
            Requested,
            /// @brief Composited into m_Image; readable once its frame ends.
            Composited,
        };

        /// @brief Removes the capture from the compositor if it is installed.
        void Uninstall();

        /// @brief The render context the frame is composited by; borrowed.
        Renderer::Context& m_Context;

        /// @brief The compositor the capture installs itself on while a request is open; borrowed.
        Renderer::ViewportCompositor& m_Compositor;

        /// @brief The image the capture composite renders into; held from composite until Take.
        Ref<Renderer::Image> m_Image;

        /// @brief The open request's settings.
        FrameCaptureRequest m_Request;

        /// @brief Where the open capture has got to.
        Stage m_Stage = Stage::Idle;

        /// @brief The frame-in-flight slot the capture was composited in.
        u32 m_Slot = 0;

        /// @brief The serial of the frame the capture was composited in.
        u64 m_FrameSerial = 0;

        /// @brief Whether the captured frame's slot has retired since it was composited.
        bool m_Retired = false;
    };
}
