#include <Veng/Capture/FrameCapture.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ViewportCompositor.h>

namespace Veng::Capture
{
    FrameCapture::FrameCapture(Renderer::Context& context, Renderer::ViewportCompositor& compositor)
        : m_Context(context), m_Compositor(compositor)
    {
    }

    FrameCapture::~FrameCapture()
    {
        Uninstall();
    }

    bool FrameCapture::IsAvailable() const
    {
        return !m_Context.IsHeadless() && m_Compositor.HasTail();
    }

    VoidResult FrameCapture::Request(const FrameCaptureRequest& request)
    {
        if (m_Context.IsHeadless())
        {
            return std::unexpected(string("a headless run composites no frame to capture"));
        }
        if (!m_Compositor.HasTail())
        {
            return std::unexpected(
                string("this application composites no frame to capture (it runs no overlay)"));
        }
        if (m_Stage != Stage::Idle)
        {
            return std::unexpected(string("a frame capture is already open"));
        }

        m_Request = request;
        m_Stage = Stage::Requested;
        m_Retired = false;
        m_Compositor.AddCaptureSink(*this);
        return {};
    }

    bool FrameCapture::IsPending() const
    {
        if (m_Stage == Stage::Requested)
        {
            return true;
        }
        if (m_Stage != Stage::Composited || m_Retired)
        {
            return false;
        }
        // The capture was recorded into frame m_FrameSerial's command buffer, which is submitted by
        // that frame's end: a later frame has begun, or none is recording.
        return m_Context.GetFrameSerial() == m_FrameSerial && m_Context.IsRecordingFrame();
    }

    bool FrameCapture::IsComplete() const
    {
        return m_Stage == Stage::Composited && m_Retired;
    }

    bool FrameCapture::IsOpen() const
    {
        return m_Stage != Stage::Idle;
    }

    Result<RgbImage> FrameCapture::Take()
    {
        if (m_Stage == Stage::Idle)
        {
            return std::unexpected(string("no frame capture is open"));
        }
        if (IsPending())
        {
            return std::unexpected(string("no frame has been composited since the capture was "
                                          "requested"));
        }

        const Ref<Renderer::Image> image = std::move(m_Image);
        Cancel();
        return ReadImageRgb8(*image);
    }

    void FrameCapture::Cancel()
    {
        Uninstall();
        m_Stage = Stage::Idle;
        m_Retired = false;
        m_Image.reset();
    }

    optional<Renderer::CaptureTarget> FrameCapture::AcquireTarget(const u32 slot,
                                                                  const uvec2 presentedExtent)
    {
        if (m_Stage != Stage::Requested)
        {
            return std::nullopt;
        }

        m_Image =
            Renderer::Image::Create(m_Context, {
                                                   .Name = "Frame Capture",
                                                   .Extent = uvec3(presentedExtent, 1u),
                                                   .Format = Renderer::Format::BGRA8Srgb,
                                                   .Usage = Renderer::ImageUsage::ColorAttachment |
                                                            Renderer::ImageUsage::TransferSrc,
                                               });
        m_Stage = Stage::Composited;
        m_Slot = slot;
        m_FrameSerial = m_Context.GetFrameSerial();

        // SrgbNonlinear into an _SRGB target whatever the display: the store encodes, so an HDR or
        // extended-linear display still yields an ordinary sRGB picture.
        return Renderer::CaptureTarget{
            .Image = m_Image,
            .ColorSpace = Renderer::DisplayColorSpace::SrgbNonlinear,
            .IncludeOverlay = m_Request.IncludeOverlay,
        };
    }

    void FrameCapture::OnSlotRetired(const u32 slot)
    {
        if (m_Stage != Stage::Composited || slot != m_Slot ||
            m_Context.GetFrameSerial() <= m_FrameSerial)
        {
            return;
        }
        m_Retired = true;
        Uninstall();
    }

    void FrameCapture::Uninstall()
    {
        m_Compositor.RemoveCaptureSink(*this);
    }
}
