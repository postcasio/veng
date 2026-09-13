#include <Veng/Capture/VideoRecorder.h>

#include "RecorderCore.h"
#include "VideoRecorderBackend.h"

#include <Veng/Audio/AudioDevice.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Backend/MetalInterop.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Time.h>

#include <ctime>
#include <utility>

namespace Veng::Capture
{
    bool CodecSupportsEncoding(const VideoCodec codec, const CaptureEncoding encoding)
    {
        // The hardware H.264 encoder has no ten-bit profile; every other codec carries both.
        return !(codec == VideoCodec::H264 && encoding == CaptureEncoding::Hdr10);
    }

    string DescribeCodec(const VideoCodec codec, const CaptureEncoding encoding)
    {
        switch (codec)
        {
        case VideoCodec::Hevc:
            return encoding == CaptureEncoding::Hdr10 ? "HEVC Main 10" : "HEVC Main";
        case VideoCodec::H264:
            return "H.264 High";
        case VideoCodec::ProRes422HQ:
            return "Apple ProRes 422 HQ";
        case VideoCodec::ProRes4444:
            return "Apple ProRes 4444";
        }
        return "unknown";
    }

    VideoRecorder::VideoRecorder(Renderer::Context& context,
                                 Renderer::ViewportCompositor& compositor,
                                 Audio::AudioDevice& device, VideoRecorderHost host)
        : m_Context(context), m_Compositor(compositor), m_Device(device), m_Host(std::move(host))
    {
        m_Core = CreateUnique<RecorderCore>(RecorderCore::Hooks{
            .Now = [] { return static_cast<f64>(Time::Now()); },
            .BeforeDrain = [this] { m_Context.WaitIdle(); },
        });

        // The subscription has no removal, so it is made once here rather than per capture — and
        // only where there is a swap chain to be invalidated at all.
        if (m_Context.IsSwapChainCaptureSupported())
        {
            m_Context.AddSwapChainInvalidationCallback([this] { Abort("swap chain changed"); });
        }
    }

    VideoRecorder::~VideoRecorder()
    {
        Stop();
        WaitForFinalize();
    }

    bool VideoRecorder::IsAvailable() const
    {
        return IsVideoRecorderBackendCompiled() && m_Context.IsExternalTextureImportSupported() &&
               !m_Context.IsHeadless();
    }

    CaptureEncoding VideoRecorder::ResolveEncoding(const CaptureEncoding requested) const
    {
        if (requested != CaptureEncoding::Auto)
        {
            return requested;
        }

        // An extended-linear (EDR) swap chain resolves to Sdr: a file most players and editors show
        // correctly, with Hdr10 one setting away.
        return m_Context.GetActiveDisplayColorSpace() == Renderer::DisplayColorSpace::Hdr10St2084
                   ? CaptureEncoding::Hdr10
                   : CaptureEncoding::Sdr;
    }

    bool VideoRecorder::Start(const VideoCaptureSettings& settings)
    {
        m_Core->PollFinalize();
        m_StartError.clear();

        const auto refuse = [this](string reason)
        {
            Log::Warn("Video capture refused: {}", reason);
            m_StartError = std::move(reason);
            return false;
        };

        if (m_Core->IsRecording())
        {
            return refuse("A capture is already recording.");
        }
        if (m_Core->IsFinalizing())
        {
            return refuse("The previous capture is still being written to disk.");
        }
        if (!IsVideoRecorderBackendCompiled())
        {
            return refuse("This platform has no video encoder.");
        }
        if (m_Context.IsHeadless())
        {
            return refuse("A headless run presents no frame to record.");
        }
        if (!m_Context.IsExternalTextureImportSupported())
        {
            return refuse("This device cannot share an encoder's memory with the renderer.");
        }

        VideoCaptureSettings resolved = settings;
        resolved.Encoding = ResolveEncoding(settings.Encoding);

        if (const VoidResult valid = ValidateCaptureSettings(resolved, m_Device.GetChannels());
            !valid)
        {
            return refuse(valid.error());
        }

        if (resolved.Lockstep && !m_Host.DriveFrameClock)
        {
            return refuse("This host's frame clock cannot be driven, so a lockstep capture is not "
                          "available.");
        }

        const uvec2 extent = m_Context.GetSwapChainExtent();
        if (extent.x == 0 || extent.y == 0)
        {
            return refuse("The window has no presentable size right now.");
        }

        const string name = resolved.Name.empty()
                                ? DefaultCaptureName(m_Host.Name, std::time(nullptr))
                                : resolved.Name;

        RecorderCoreInfo info{
            .Backend = CreateVideoRecorderBackend(),
            .Settings = resolved,
            .Open =
                {
                    .File = ResolveVideoCapturePath(name),
                    .Extent = extent,
                    .Device = m_Context.GetExternalDevice(),
                    .Encoding = resolved.Encoding,
                    .Codec = resolved.Codec,
                    .BitsPerSecond = DeriveBitsPerSecond(resolved, extent),
                    .FrameRate = resolved.FrameRate,
                    .Audio = resolved.Audio,
                    .SampleRate = m_Device.GetSampleRate(),
                    .Channels = m_Device.GetChannels(),
                    // The frames in flight plus a small margin: the encoder holding more buffers
                    // than that is behind, and the pool refusing to vend is the back-pressure that
                    // turns the lag into a bounded wait rather than unbounded surface growth.
                    .PoolAllocationThreshold = m_Context.GetMaxFramesInFlight() + 4,
                },
            .TapBlockFrames = Audio::AudioDevice::TapBlockFrames,
        };

        if (const VoidResult opened = m_Core->Open(std::move(info)); !opened)
        {
            return refuse(opened.error());
        }

        m_Extent = extent;
        m_Imports.clear();
        m_NeedsDetach = false;

        m_Compositor.SetCaptureSink(this);
        m_SinkInstalled = true;

        if (resolved.Audio != AudioTrack::None)
        {
            m_Device.SetBlockTap(
                [this](std::span<const f32> interleaved, const u32 frames)
                {
                    const u64 overruns = m_Device.GetTapOverruns();
                    const u64 lost = overruns - m_LastTapOverruns;
                    m_LastTapOverruns = overruns;
                    m_Core->PushAudio(interleaved, frames, lost);
                });
            // Installing a tap resets the device's overrun count, so the baseline is zero.
            m_LastTapOverruns = 0;
            m_TapInstalled = true;
        }

        if (resolved.Lockstep)
        {
            m_Host.DriveFrameClock(1.0f / static_cast<f32>(resolved.FrameRate));
            m_DroveFrameClock = true;
        }

        Log::Info("Video capture recording {} at {}x{} to {}",
                  DescribeCodec(resolved.Codec, resolved.Encoding), extent.x, extent.y,
                  m_Core->GetState().Path.string());
        return true;
    }

    void VideoRecorder::Detach()
    {
        if (m_SinkInstalled)
        {
            m_Compositor.SetCaptureSink(nullptr);
            m_SinkInstalled = false;
        }
        if (m_TapInstalled)
        {
            m_Device.SetBlockTap(nullptr);
            m_TapInstalled = false;
        }
        if (m_DroveFrameClock)
        {
            if (m_Host.ReleaseFrameClock)
            {
                m_Host.ReleaseFrameClock();
            }
            m_DroveFrameClock = false;
        }
        m_NeedsDetach = false;
        m_Imports.clear();
    }

    void VideoRecorder::Stop()
    {
        if (m_Core->IsRecording())
        {
            Detach();
            m_Core->Finish();
        }
        else if (m_NeedsDetach)
        {
            Detach();
        }

        m_Core->PollFinalize();
    }

    void VideoRecorder::Abort(string reason)
    {
        if (!m_Core->IsRecording())
        {
            return;
        }

        m_Core->Abort(std::move(reason));
        Detach();
        m_Core->PollFinalize();
    }

    void VideoRecorder::WaitForFinalize()
    {
        m_Core->WaitForFinalize();
    }

    bool VideoRecorder::IsRecording() const
    {
        return m_Core->IsRecording();
    }

    VideoCaptureState VideoRecorder::GetState() const
    {
        // The status is polled here because a finalizing capture has no frames left to drive it:
        // reading the state is the one thing that still happens once recording has stopped.
        m_Core->PollFinalize();

        VideoCaptureState state = m_Core->GetState();
        if (!m_StartError.empty())
        {
            state.LastError = m_StartError;
        }
        return state;
    }

    optional<Renderer::CaptureTarget> VideoRecorder::AcquireTarget(const u32 slot,
                                                                   const uvec2 presentedExtent)
    {
        if (!m_Core->IsRecording())
        {
            return std::nullopt;
        }

        if (presentedExtent != m_Extent)
        {
            m_Core->Abort("the presented frame changed size");
            m_NeedsDetach = true;
            return std::nullopt;
        }

        const optional<PixelBufferHandle> buffer = m_Core->Acquire(slot);
        if (!buffer)
        {
            // Either the budget is spent or the never-drop wait ended the capture; both leave the
            // sink to be removed at the next safe point, since this runs inside the composite.
            m_NeedsDetach = true;
            return std::nullopt;
        }

        Ref<Renderer::Image>& imported = m_Imports[buffer->Surface];
        if (!imported)
        {
            imported = Renderer::Backend::ImportExternalTexture(m_Context, buffer->Texture,
                                                                "Video Capture Target");
        }

        if (!imported)
        {
            m_Imports.erase(buffer->Surface);
            m_Core->Abort("the encoder's frame buffer could not be imported");
            m_NeedsDetach = true;
            return std::nullopt;
        }

        return Renderer::CaptureTarget{
            .Image = imported,
            .ColorSpace = m_Core->GetSettings().Encoding == CaptureEncoding::Hdr10
                              ? Renderer::DisplayColorSpace::Hdr10St2084
                              : Renderer::DisplayColorSpace::SrgbNonlinear,
            .IncludeOverlay = m_Core->GetSettings().IncludeOverlay,
        };
    }

    void VideoRecorder::OnSlotRetired(const u32 slot)
    {
        m_Core->Retire(slot);

        if (m_Core->WantsStop() || m_NeedsDetach)
        {
            Stop();
        }
    }
}
