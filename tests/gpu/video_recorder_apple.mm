// The recorder's encoder backend and the import it feeds, proven end to end against a real file.
//
// Every context in this band is headless and the capture composite runs only windowed, so what this
// case can reach is the backend's half of the contract: the writer's pool vends buffers the renderer
// can import and render into, those buffers encode, and the file that comes back carries the
// picture, the sound and the colour tags the encoding asked for. The composite into the sink is not
// reachable here — see engine/src/Capture/CLAUDE.md.
//
// Skips with a message rather than failing when the hardware encoder is unavailable (no logged-in
// window server session), which is the CI case rather than a defect.

#include <doctest/doctest.h>

#include <Capture/VideoRecorderBackend.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <Veng/Renderer/Backend/MetalInterop.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>

#include <gpu/fixture.h>
#include <support/TempPath.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace Veng;
using namespace Veng::Capture;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Width = 320;
    constexpr u32 Height = 180;
    constexpr u32 FrameCount = 10;
    constexpr u32 FrameRate = 60;
    constexpr u32 AudioBlockFrames = 800;
    constexpr u32 SampleRate = 48000;
    constexpr u32 Channels = 2;
    // Sound blocks appended before any picture exists. A writer interleaving its tracks would hold
    // the sound input until the picture caught up, and each append would run out its wait bound.
    constexpr u32 AudioBlocksAhead = 4;

    /// @brief Whether a hardware HEVC encoder can be created in this session.
    bool HasHardwareEncoder()
    {
        VTCompressionSessionRef session = nullptr;
        NSDictionary* specification = @{
            (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES,
        };
        const OSStatus made = VTCompressionSessionCreate(
            kCFAllocatorDefault, Width, Height, kCMVideoCodecType_HEVC,
            (__bridge CFDictionaryRef)specification, nullptr, nullptr, nullptr, nullptr, &session);
        if (session != nullptr)
        {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
        return made == noErr;
    }

    /// @brief The linear colour frame @p frame is cleared to.
    ClearColor ClearFor(const u32 frame)
    {
        return frame == 0 ? ClearColor{.R = 0.25f, .G = 0.05f, .B = 0.60f, .A = 1.0f}
                          : ClearColor{.R = 0.80f, .G = 0.40f, .B = 0.10f, .A = 1.0f};
    }

    /// @brief Encodes a linear value with the sRGB transfer function, as an 8-bit target's store does.
    f64 SrgbEncode(const f64 linear)
    {
        return linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    }

    /// @brief Records one clear of @p view through a one-pass graph.
    void RecordClear(Context& context, CommandBuffer& cmd, const Ref<ImageView>& view,
                     const ClearColor& clear)
    {
        RenderGraph graph(context);
        const ResourceId target = graph.Import("Target");
        graph.AddPass("clear")
            .Color({
                .Resource = target,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = clear,
            })
            .Execute([](PassContext&) {});
        const RenderGraph::ImportBinding binding{.Id = target, .View = view};
        graph.Compile()->Execute(cmd, {&binding, 1});
    }

    /// @brief A decoded frame's mean channel values, 0-255.
    struct MeanColor
    {
        /// @brief Mean blue, as stored.
        f64 B = 0.0;
        /// @brief Mean green, as stored.
        f64 G = 0.0;
        /// @brief Mean red, as stored.
        f64 R = 0.0;
    };

    /// @brief Averages a BGRA pixel buffer's three colour channels.
    MeanColor MeanOf(CVPixelBufferRef buffer)
    {
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        const u8* base = static_cast<const u8*>(CVPixelBufferGetBaseAddress(buffer));
        const usize stride = CVPixelBufferGetBytesPerRow(buffer);
        const usize width = CVPixelBufferGetWidth(buffer);
        const usize height = CVPixelBufferGetHeight(buffer);

        f64 blue = 0.0;
        f64 green = 0.0;
        f64 red = 0.0;
        for (usize y = 0; y < height; ++y)
        {
            const u8* row = base + (y * stride);
            for (usize x = 0; x < width; ++x)
            {
                blue += row[(x * 4) + 0];
                green += row[(x * 4) + 1];
                red += row[(x * 4) + 2];
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

        const f64 texels = static_cast<f64>(width * height);
        return MeanColor{.B = blue / texels, .G = green / texels, .R = red / texels};
    }

    /// @brief Loads an asset's tracks of one media type without the deprecated blocking accessor.
    NSArray<AVAssetTrack*>* TracksOf(AVAsset* asset, AVMediaType type)
    {
        __block NSArray<AVAssetTrack*>* loaded = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [asset loadTracksWithMediaType:type
                    completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*) {
                      loaded = [tracks retain];
                      dispatch_semaphore_signal(done);
                    }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        return [loaded autorelease];
    }

    /// @brief Writes one capture at @p encoding and returns the file it wrote.
    ///
    /// Ten frames cleared to a known colour, imported through the renderer and rendered into, plus
    /// ten blocks of a tone — the whole of the backend's contract in one pass.
    /// @param context   The render context the buffers are imported into.
    /// @param encoding  The capture's encoding.
    /// @param file      The file to write.
    /// @return True when every step took.
    bool WriteCapture(Context& context, const CaptureEncoding encoding, const path& file)
    {
        const Unique<VideoRecorderBackend> backend = CreateVideoRecorderBackend();
        if (!backend)
        {
            return false;
        }

        const VoidResult opened = backend->Open(BackendOpenInfo{
            .File = file,
            .Extent = uvec2{Width, Height},
            .Device = context.GetExternalDevice(),
            .Encoding = encoding,
            .Codec = VideoCodec::Hevc,
            .BitsPerSecond = 4000000,
            .FrameRate = FrameRate,
            .Audio = AudioTrack::Pcm,
            .SampleRate = SampleRate,
            .Channels = Channels,
            .PoolAllocationThreshold = 8,
        });
        if (!opened)
        {
            MESSAGE("the writer refused to open: " << opened.error());
            return false;
        }

        vector<f32> tone(AudioBlockFrames * Channels, 0.0f);
        for (u32 sample = 0; sample < AudioBlockFrames; ++sample)
        {
            const f32 value = 0.25f * std::sin(static_cast<f32>(sample) * 0.05f);
            tone[(sample * Channels) + 0] = value;
            tone[(sample * Channels) + 1] = value;
        }

        // The recorder appends a frame's sound before that frame's picture, by several frames: the
        // sound input must accept those blocks without waiting on a picture. Measured as a whole
        // and asserted once — an interleaving writer would cost the full bound per block.
        const auto aheadStarted = std::chrono::steady_clock::now();
        for (u32 block = 0; block < AudioBlocksAhead; ++block)
        {
            const VoidResult sound = backend->AppendAudio(
                tone, AudioBlockFrames, static_cast<u64>(block) * AudioBlockFrames);
            if (!sound)
            {
                MESSAGE("a sound block ahead of any picture would not encode: " << sound.error());
                return false;
            }
        }
        const f64 aheadSeconds =
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - aheadStarted).count();
        CHECK(aheadSeconds < 0.5);

        map<void*, Ref<Image>> imports;
        map<void*, Ref<ImageView>> views;

        for (u32 frame = 0; frame < FrameCount; ++frame)
        {
            std::expected<PixelBufferHandle, AcquireRefusal> taken;
            for (u32 attempt = 0; attempt < 4000; ++attempt)
            {
                taken = backend->AcquirePixelBuffer();
                if (taken || taken.error().Reason == AcquireWait::WriterFailed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            if (!taken)
            {
                MESSAGE("the encoder never vended a buffer: " << taken.error().Error);
                return false;
            }

            Ref<Image>& imported = imports[taken->Surface];
            if (!imported)
            {
                imported =
                    Backend::ImportExternalTexture(context, taken->Texture, "Capture Buffer");
                if (!imported)
                {
                    return false;
                }
                views[taken->Surface] =
                    ImageView::Create(context, {.Name = "Capture Buffer View", .Image = imported});
            }

            context.ImmediateCommands([&](CommandBuffer& cmd)
                                      { RecordClear(context, cmd, views[taken->Surface],
                                                    ClearFor(frame)); });

            const i64 pts = static_cast<i64>(frame) * (VideoTimescale / FrameRate);
            const VoidResult appended = backend->AppendVideo(*taken, pts);
            backend->ReleasePixelBuffer(*taken);
            if (!appended)
            {
                MESSAGE("a frame would not encode: " << appended.error());
                return false;
            }

            const VoidResult sound = backend->AppendAudio(
                tone, AudioBlockFrames,
                static_cast<u64>(frame + AudioBlocksAhead) * AudioBlockFrames);
            if (!sound)
            {
                MESSAGE("a sound block would not encode: " << sound.error());
                return false;
            }
        }

        context.WaitIdle();
        views.clear();
        imports.clear();

        const Ref<std::atomic<bool>> committed = std::make_shared<std::atomic<bool>>(false);
        backend->Finish([committed] { committed->store(true, std::memory_order_release); });
        while (!committed->load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        return backend->FileBytes() > 0;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "video recorder backend: a capture encodes to a playable QuickTime movie")
{
    REQUIRE(Context.IsExternalTextureImportSupported());
    if (!HasHardwareEncoder())
    {
        MESSAGE("skipped: no hardware video encoder in this session");
        return;
    }

    const path file = TestSupport::TempDir() / "veng_capture_sdr.mov";
    REQUIRE(WriteCapture(Context, CaptureEncoding::Sdr, file));

    u32 decoded = 0;
    MeanColor first;
    MeanColor last;
    f64 videoSeconds = 0.0;
    f64 audioSeconds = 0.0;
    f64 audioRate = 0.0;
    usize videoTracks = 0;
    usize audioTracks = 0;

    @autoreleasepool
    {
        AVURLAsset* asset = [AVURLAsset
            URLAssetWithURL:[NSURL fileURLWithPath:[NSString
                                                       stringWithUTF8String:file.string().c_str()]]
                    options:nil];

        NSArray<AVAssetTrack*>* video = TracksOf(asset, AVMediaTypeVideo);
        NSArray<AVAssetTrack*>* audio = TracksOf(asset, AVMediaTypeAudio);
        videoTracks = video.count;
        audioTracks = audio.count;
        REQUIRE(videoTracks == 1);
        REQUIRE(audioTracks == 1);

        videoSeconds = CMTimeGetSeconds(video[0].timeRange.duration);
        audioSeconds = CMTimeGetSeconds(audio[0].timeRange.duration);

        auto description = static_cast<CMAudioFormatDescriptionRef>(
            (__bridge CMFormatDescriptionRef)audio[0].formatDescriptions[0]);
        const AudioStreamBasicDescription* stream =
            CMAudioFormatDescriptionGetStreamBasicDescription(description);
        audioRate = stream != nullptr ? stream->mSampleRate : 0.0;

        NSError* error = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
        REQUIRE(reader != nil);
        AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput
            assetReaderTrackOutputWithTrack:video[0]
                             outputSettings:@{
                                 (id)kCVPixelBufferPixelFormatTypeKey :
                                     @(kCVPixelFormatType_32BGRA),
                             }];
        [reader addOutput:output];
        REQUIRE([reader startReading]);

        for (;;)
        {
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            if (sample == nullptr)
            {
                break;
            }
            CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
            if (image != nullptr)
            {
                if (decoded == 0)
                {
                    first = MeanOf(image);
                }
                last = MeanOf(image);
            }
            ++decoded;
            CFRelease(sample);
        }
    }

    CHECK(decoded == FrameCount);
    // The last frame's own duration is the writer's to choose, so the track's span is the ten
    // frames' presentation plus at most one more.
    CHECK(videoSeconds >= static_cast<f64>(FrameCount - 1) / FrameRate - 0.001);
    CHECK(videoSeconds <= static_cast<f64>(FrameCount) / FrameRate + 0.001);
    CHECK(audioRate == doctest::Approx(SampleRate));
    CHECK(audioSeconds ==
          doctest::Approx(static_cast<f64>((FrameCount + AudioBlocksAhead) * AudioBlockFrames) /
                          SampleRate)
              .epsilon(0.01));

    // The clear was linear and the target's store encodes it, so the stored byte is the sRGB value
    // — the property an eight-bit capture of a linear frame depends on.
    const f64 tolerance = 10.0;
    CHECK(std::abs(first.R - (SrgbEncode(ClearFor(0).R) * 255.0)) < tolerance);
    CHECK(std::abs(first.G - (SrgbEncode(ClearFor(0).G) * 255.0)) < tolerance);
    CHECK(std::abs(first.B - (SrgbEncode(ClearFor(0).B) * 255.0)) < tolerance);
    CHECK(std::abs(last.R - (SrgbEncode(ClearFor(9).R) * 255.0)) < tolerance);
    CHECK(std::abs(last.G - (SrgbEncode(ClearFor(9).G) * 255.0)) < tolerance);
    CHECK(std::abs(last.B - (SrgbEncode(ClearFor(9).B) * 255.0)) < tolerance);

    std::error_code removed;
    std::filesystem::remove(file, removed);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "video recorder backend: an HDR10 capture carries Main 10 and PQ tags")
{
    REQUIRE(Context.IsExternalTextureImportSupported());
    if (!HasHardwareEncoder())
    {
        MESSAGE("skipped: no hardware video encoder in this session");
        return;
    }

    const path file = TestSupport::TempDir() / "veng_capture_hdr10.mov";
    REQUIRE(WriteCapture(Context, CaptureEncoding::Hdr10, file));

    bool hevc = false;
    bool main10 = false;
    bool rec2020 = false;
    bool pq = false;

    @autoreleasepool
    {
        AVURLAsset* asset = [AVURLAsset
            URLAssetWithURL:[NSURL fileURLWithPath:[NSString
                                                       stringWithUTF8String:file.string().c_str()]]
                    options:nil];
        NSArray<AVAssetTrack*>* video = TracksOf(asset, AVMediaTypeVideo);
        REQUIRE(video.count == 1);

        CMFormatDescriptionRef description =
            (__bridge CMFormatDescriptionRef)video[0].formatDescriptions[0];
        hevc = CMFormatDescriptionGetMediaSubType(description) == kCMVideoCodecType_HEVC;

        auto primaries = static_cast<CFStringRef>(CMFormatDescriptionGetExtension(
            description, kCMFormatDescriptionExtension_ColorPrimaries));
        auto transfer = static_cast<CFStringRef>(CMFormatDescriptionGetExtension(
            description, kCMFormatDescriptionExtension_TransferFunction));
        rec2020 = primaries != nullptr &&
                  CFEqual(primaries, kCMFormatDescriptionColorPrimaries_ITU_R_2020);
        pq = transfer != nullptr &&
             CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ);

        // The profile lives in the hvcC configuration record: byte 1 carries profile_space,
        // tier_flag and general_profile_idc, which is 2 for Main 10 and 1 for Main.
        auto atoms = static_cast<CFDictionaryRef>(CMFormatDescriptionGetExtension(
            description, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms));
        if (atoms != nullptr)
        {
            auto record = static_cast<CFDataRef>(CFDictionaryGetValue(atoms, CFSTR("hvcC")));
            if (record != nullptr && CFDataGetLength(record) >= 2)
            {
                main10 = (CFDataGetBytePtr(record)[1] & 0x1F) == 2;
            }
        }
    }

    CHECK(hevc);
    CHECK(main10);
    CHECK(rec2020);
    CHECK(pq);

    std::error_code removed;
    std::filesystem::remove(file, removed);
}
