#pragma once

// Device-free request logic for the presented-frame mirror, kept apart from Context.cpp so the
// decision of which frame ends pay the mirror blit is unit-tested without a swap chain.

#include <Veng/Veng.h>

namespace Veng::Renderer::Backend
{
    /// @brief The one-shot request latch deciding which frame ends mirror the presented frame.
    ///
    /// Context::RequestPresentedFrameCapture sets it and EndFrame takes it, so a frame end
    /// mirrors only when something asked since the last one did; any number of requests before
    /// that frame end coalesce into it.
    class PresentedFrameCaptureLatch
    {
    public:
        /// @brief Records a request for the next frame end to mirror its frame.
        void Request() { m_Pending = true; }

        /// @brief Whether a request is waiting for the frame end that services it.
        [[nodiscard]] bool IsPending() const { return m_Pending; }

        /// @brief Called once per presenting frame end; consumes the request if there is one.
        /// @return True when this frame end is to mirror its frame.
        [[nodiscard]] bool TakeForFrame()
        {
            const bool take = m_Pending;
            m_Pending = false;
            return take;
        }

    private:
        /// @brief Set by Request, cleared by the frame end that takes it.
        bool m_Pending = false;
    };
}
