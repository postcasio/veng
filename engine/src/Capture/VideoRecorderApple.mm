// The recorder's Apple backend: one AVAssetWriter writing a QuickTime movie, its pixel-buffer
// adaptor's pool supplying the frames the renderer composites into, and VideoToolbox encoding them.
//
// Non-ARC (the default for this tree's Objective-C++), so every object this holds across a call is
// retained by hand and released in the destructor. Nothing Vulkan appears here: the buffers' memory
// reaches the renderer as an MTLTexture handle the import reads its own extent and format off.

#include "VideoRecorderBackend.h"

#include <Veng/Assert.h>
#include <Veng/Log.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

namespace Veng::Capture
{
    namespace
    {
        /// @brief How long an append may wait for a track input to accept more data, in seconds.
        constexpr f64 InputReadyBoundSeconds = 2.0;

        /// @brief Returns the pixel-buffer format @p encoding's frames are vended in.
        OSType PoolPixelFormat(const CaptureEncoding encoding)
        {
            return encoding == CaptureEncoding::Hdr10 ? kCVPixelFormatType_ARGB2101010LEPacked
                                                      : kCVPixelFormatType_32BGRA;
        }

        /// @brief Returns the Metal format a buffer of @p encoding is wrapped as.
        MTLPixelFormat BufferTextureFormat(const CaptureEncoding encoding)
        {
            return encoding == CaptureEncoding::Hdr10 ? MTLPixelFormatBGR10A2Unorm
                                                      : MTLPixelFormatBGRA8Unorm_sRGB;
        }

        /// @brief Returns the codec identifier AVFoundation names @p codec by.
        NSString* CodecIdentifier(const VideoCodec codec)
        {
            switch (codec)
            {
            case VideoCodec::Hevc:
                return AVVideoCodecTypeHEVC;
            case VideoCodec::H264:
                return AVVideoCodecTypeH264;
            case VideoCodec::ProRes422HQ:
                return AVVideoCodecTypeAppleProRes422HQ;
            case VideoCodec::ProRes4444:
                return AVVideoCodecTypeAppleProRes4444;
            }
            return AVVideoCodecTypeHEVC;
        }

        /// @brief Returns the colour tags a player reads @p encoding's picture through.
        NSDictionary* ColorProperties(const CaptureEncoding encoding)
        {
            if (encoding == CaptureEncoding::Hdr10)
            {
                return @{
                    AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_2020,
                    AVVideoTransferFunctionKey : AVVideoTransferFunction_SMPTE_ST_2084_PQ,
                    AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_2020,
                };
            }
            return @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_IEC_sRGB,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            };
        }

        /// @brief Reads an NSError's description as an engine string.
        string Describe(NSError* error)
        {
            if (error == nil)
            {
                return "the writer gave no reason";
            }
            const char* text = [[error localizedDescription] UTF8String];
            return text != nullptr ? string(text) : string("the writer gave no reason");
        }

        /// @brief The Apple backend: an AVAssetWriter, its buffer pool, and its two track inputs.
        class AppleVideoRecorderBackend final : public VideoRecorderBackend
        {
        public:
            ~AppleVideoRecorderBackend() override
            {
                for (const auto& [surface, texture] : m_Textures)
                {
                    [static_cast<id<MTLTexture>>(texture) release];
                }
                m_Textures.clear();

                if (m_AudioFormat != nullptr)
                {
                    CFRelease(m_AudioFormat);
                }
                [m_Adaptor release];
                [m_VideoInput release];
                [m_AudioInput release];
                [m_Writer release];
                [m_Device release];
            }

            VoidResult Open(const BackendOpenInfo& info) override;

            std::expected<PixelBufferHandle, AcquireRefusal> AcquirePixelBuffer() override;

            VoidResult AppendVideo(const PixelBufferHandle& handle, const i64 ptsTicks) override
            {
                if (!WaitInputReady(m_VideoInput))
                {
                    return std::unexpected(Describe(m_Writer.error));
                }

                const BOOL appended =
                    [m_Adaptor appendPixelBuffer:static_cast<CVPixelBufferRef>(handle.Buffer)
                            withPresentationTime:CMTimeMake(ptsTicks, VideoTimescale)];
                if (!appended)
                {
                    return std::unexpected(Describe(m_Writer.error));
                }
                return {};
            }

            void ReleasePixelBuffer(const PixelBufferHandle& handle) override
            {
                if (handle.Buffer != nullptr)
                {
                    CFRelease(static_cast<CVPixelBufferRef>(handle.Buffer));
                }
            }

            VoidResult AppendAudio(std::span<const f32> interleaved, u32 frames,
                                   u64 ptsFrames) override;

            void Finish(function<void()> completion) override;

            [[nodiscard]] u64 FileBytes() const override
            {
                std::error_code failed;
                const std::uintmax_t size = std::filesystem::file_size(m_File, failed);
                return failed ? 0ULL : static_cast<u64>(size);
            }

            [[nodiscard]] bool IsWriterFailed() const override
            {
                return m_Writer != nil && m_Writer.status == AVAssetWriterStatusFailed;
            }

        private:
            /// @brief Waits, bounded, for @p input to accept more data; false when the writer failed.
            /// @param input  The track input to wait on.
            /// @return True when the input is ready.
            bool WaitInputReady(AVAssetWriterInput* input) const;

            /// @brief Returns the texture wrapping @p surface, creating and caching it on first use.
            /// @param surface  The buffer's shared surface.
            /// @return The texture, or nil when one could not be made.
            id<MTLTexture> TextureFor(IOSurfaceRef surface);

            /// @brief The writer owning the file and its tracks.
            AVAssetWriter* m_Writer = nil;
            /// @brief The picture track's input.
            AVAssetWriterInput* m_VideoInput = nil;
            /// @brief The sound track's input; nil when the file carries no audio.
            AVAssetWriterInput* m_AudioInput = nil;
            /// @brief The adaptor owning the pool the recorder's frames come from.
            AVAssetWriterInputPixelBufferAdaptor* m_Adaptor = nil;
            /// @brief The sound track's sample format, reused for every appended block.
            CMAudioFormatDescriptionRef m_AudioFormat = nullptr;
            /// @brief The graphics device the buffers' textures are created on.
            id<MTLDevice> m_Device = nil;
            /// @brief Textures wrapping the pool's surfaces, keyed by surface; retained.
            map<void*, void*> m_Textures;
            /// @brief The pixel format every vended buffer must carry.
            OSType m_PixelFormat = kCVPixelFormatType_32BGRA;
            /// @brief The Metal format the buffers' textures are wrapped as.
            MTLPixelFormat m_TextureFormat = MTLPixelFormatBGRA8Unorm_sRGB;
            /// @brief Buffers the pool may have outstanding before it refuses to vend another.
            u32 m_Threshold = 8;
            /// @brief The sound track's channel count.
            u32 m_Channels = 2;
            /// @brief The sound track's sample rate in Hz.
            u32 m_SampleRate = 48000;
            /// @brief The file being written.
            path m_File;
            /// @brief Whether Finish has already been called.
            bool m_Finishing = false;
        };

        VoidResult AppleVideoRecorderBackend::Open(const BackendOpenInfo& info)
        {
            @autoreleasepool
            {
                m_File = info.File;
                m_PixelFormat = PoolPixelFormat(info.Encoding);
                m_TextureFormat = BufferTextureFormat(info.Encoding);
                m_Threshold = info.PoolAllocationThreshold;
                m_Channels = info.Channels;
                m_SampleRate = info.SampleRate;

                if (info.Device == nullptr)
                {
                    return std::unexpected("The renderer exported no graphics device to share with "
                                           "the encoder.");
                }
                m_Device = [static_cast<id<MTLDevice>>(info.Device) retain];

                NSURL* url = [NSURL
                    fileURLWithPath:[NSString stringWithUTF8String:info.File.string().c_str()]];
                [[NSFileManager defaultManager] removeItemAtURL:url error:nil];

                NSError* error = nil;
                // QuickTime unconditionally: it is the one container that carries both ProRes and
                // uncompressed float PCM.
                AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url
                                                                 fileType:AVFileTypeQuickTimeMovie
                                                                    error:&error];
                if (writer == nil)
                {
                    return std::unexpected(
                        fmt::format("The capture file could not be created: {}", Describe(error)));
                }
                m_Writer = [writer retain];

                NSMutableDictionary* videoSettings = [NSMutableDictionary dictionary];
                videoSettings[AVVideoCodecKey] = CodecIdentifier(info.Codec);
                videoSettings[AVVideoWidthKey] = @(info.Extent.x);
                videoSettings[AVVideoHeightKey] = @(info.Extent.y);
                videoSettings[AVVideoColorPropertiesKey] = ColorProperties(info.Encoding);

                if (info.Codec == VideoCodec::Hevc || info.Codec == VideoCodec::H264)
                {
                    NSMutableDictionary* compression = [NSMutableDictionary dictionary];
                    compression[AVVideoAverageBitRateKey] =
                        @(static_cast<NSInteger>(info.BitsPerSecond));
                    compression[AVVideoExpectedSourceFrameRateKey] = @(info.FrameRate);
                    if (info.Codec == VideoCodec::Hevc)
                    {
                        compression[AVVideoProfileLevelKey] =
                            info.Encoding == CaptureEncoding::Hdr10
                                ? static_cast<id>(kVTProfileLevel_HEVC_Main10_AutoLevel)
                                : static_cast<id>(kVTProfileLevel_HEVC_Main_AutoLevel);
                    }
                    else
                    {
                        compression[AVVideoProfileLevelKey] = AVVideoProfileLevelH264HighAutoLevel;
                    }
                    videoSettings[AVVideoCompressionPropertiesKey] = compression;
                }

                AVAssetWriterInput* videoInput =
                    [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                      outputSettings:videoSettings];
                if (videoInput == nil)
                {
                    return std::unexpected("The encoder refused the requested picture settings.");
                }
                // A real-time capture's frames arrive at wall cadence, which the writer paces itself
                // against; a lockstep one hands them over as fast as it can and must not be paced.
                videoInput.expectsMediaDataInRealTime = info.RealTime ? YES : NO;
                m_VideoInput = [videoInput retain];

                NSDictionary* sourceAttributes = @{
                    (id)kCVPixelBufferPixelFormatTypeKey : @(m_PixelFormat),
                    (id)kCVPixelBufferWidthKey : @(info.Extent.x),
                    (id)kCVPixelBufferHeightKey : @(info.Extent.y),
                    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
                };
                AVAssetWriterInputPixelBufferAdaptor* adaptor =
                    [AVAssetWriterInputPixelBufferAdaptor
                        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                                   sourcePixelBufferAttributes:sourceAttributes];
                m_Adaptor = [adaptor retain];

                if (![m_Writer canAddInput:videoInput])
                {
                    return std::unexpected("The writer refused the picture track.");
                }
                [m_Writer addInput:videoInput];

                if (info.Audio != AudioTrack::None)
                {
                    AudioChannelLayout layout{};
                    layout.mChannelLayoutTag = info.Channels == 1 ? kAudioChannelLayoutTag_Mono
                                                                  : kAudioChannelLayoutTag_Stereo;
                    NSData* layoutData = [NSData dataWithBytes:&layout length:sizeof(layout)];

                    NSDictionary* audioSettings =
                        info.Audio == AudioTrack::Pcm
                            ? @{
                                  AVFormatIDKey : @(kAudioFormatLinearPCM),
                                  AVSampleRateKey : @(info.SampleRate),
                                  AVNumberOfChannelsKey : @(info.Channels),
                                  AVChannelLayoutKey : layoutData,
                                  AVLinearPCMBitDepthKey : @32,
                                  AVLinearPCMIsFloatKey : @YES,
                                  AVLinearPCMIsBigEndianKey : @NO,
                                  AVLinearPCMIsNonInterleaved : @NO,
                              }
                            : @{
                                  AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                                  AVSampleRateKey : @(info.SampleRate),
                                  AVNumberOfChannelsKey : @(info.Channels),
                                  AVChannelLayoutKey : layoutData,
                                  AVEncoderBitRateKey : @(192000),
                              };

                    AVAssetWriterInput* audioInput =
                        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                          outputSettings:audioSettings];
                    if (audioInput == nil || ![m_Writer canAddInput:audioInput])
                    {
                        return std::unexpected("The writer refused the sound track.");
                    }
                    audioInput.expectsMediaDataInRealTime = info.RealTime ? YES : NO;
                    m_AudioInput = [audioInput retain];
                    [m_Writer addInput:audioInput];

                    AudioStreamBasicDescription description{};
                    description.mSampleRate = info.SampleRate;
                    description.mFormatID = kAudioFormatLinearPCM;
                    description.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                    description.mFramesPerPacket = 1;
                    description.mChannelsPerFrame = info.Channels;
                    description.mBitsPerChannel = 32;
                    description.mBytesPerFrame = info.Channels * sizeof(f32);
                    description.mBytesPerPacket = description.mBytesPerFrame;

                    const OSStatus made = CMAudioFormatDescriptionCreate(
                        kCFAllocatorDefault, &description, 0, nullptr, 0, nullptr, nullptr,
                        &m_AudioFormat);
                    if (made != noErr)
                    {
                        return std::unexpected(fmt::format(
                            "The sound track's sample format could not be described ({}).",
                            static_cast<i32>(made)));
                    }
                }

                if (![m_Writer startWriting])
                {
                    return std::unexpected(fmt::format("The capture could not start: {}",
                                                       Describe(m_Writer.error)));
                }
                // Every timestamp the recorder produces is rebased to the capture's origin, so the
                // session starts at zero and the file's first frame is its own beginning.
                [m_Writer startSessionAtSourceTime:kCMTimeZero];
            }

            return {};
        }

        bool AppleVideoRecorderBackend::WaitInputReady(AVAssetWriterInput* input) const
        {
            const auto started = std::chrono::steady_clock::now();
            while (!input.isReadyForMoreMediaData)
            {
                if (m_Writer.status != AVAssetWriterStatusWriting)
                {
                    return false;
                }
                if (std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count() >=
                    InputReadyBoundSeconds)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            return true;
        }

        id<MTLTexture> AppleVideoRecorderBackend::TextureFor(IOSurfaceRef surface)
        {
            const auto cached = m_Textures.find(static_cast<void*>(surface));
            if (cached != m_Textures.end())
            {
                return static_cast<id<MTLTexture>>(cached->second);
            }

            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:m_TextureFormat
                                             width:IOSurfaceGetWidth(surface)
                                            height:IOSurfaceGetHeight(surface)
                                         mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            descriptor.storageMode = MTLStorageModeShared;

            id<MTLTexture> texture = [m_Device newTextureWithDescriptor:descriptor
                                                              iosurface:surface
                                                                  plane:0];
            if (texture == nil)
            {
                return nil;
            }

            m_Textures.emplace(static_cast<void*>(surface), static_cast<void*>(texture));
            return texture;
        }

        std::expected<PixelBufferHandle, AcquireRefusal>
        AppleVideoRecorderBackend::AcquirePixelBuffer()
        {
            if (m_Writer.status == AVAssetWriterStatusFailed)
            {
                return std::unexpected(AcquireRefusal{.Reason = AcquireWait::WriterFailed,
                                                      .Error = Describe(m_Writer.error)});
            }
            if (m_Writer.status != AVAssetWriterStatusWriting)
            {
                return std::unexpected(AcquireRefusal{
                    .Reason = AcquireWait::WriterFailed,
                    .Error = "the writer stopped accepting a capture"});
            }
            if (!m_VideoInput.isReadyForMoreMediaData)
            {
                return std::unexpected(AcquireRefusal{.Reason = AcquireWait::WriterNotReady});
            }

            // Re-read per acquire: the adaptor builds its pool lazily and was seen to replace it
            // mid-run, and a stale pool vends buffers of the wrong shape with no error.
            CVPixelBufferPoolRef pool = m_Adaptor.pixelBufferPool;
            if (pool == nullptr)
            {
                return std::unexpected(AcquireRefusal{.Reason = AcquireWait::WriterNotReady});
            }

            CVPixelBufferRef buffer = nullptr;
            CVReturn taken = kCVReturnSuccess;
            @autoreleasepool
            {
                NSDictionary* auxiliary =
                    @{(id)kCVPixelBufferPoolAllocationThresholdKey : @(m_Threshold)};
                taken = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
                    kCFAllocatorDefault, pool, (__bridge CFDictionaryRef)auxiliary, &buffer);
            }

            if (taken == kCVReturnWouldExceedAllocationThreshold)
            {
                return std::unexpected(AcquireRefusal{.Reason = AcquireWait::PoolAtThreshold});
            }
            if (taken != kCVReturnSuccess || buffer == nullptr)
            {
                return std::unexpected(AcquireRefusal{
                    .Reason = AcquireWait::WriterFailed,
                    .Error = fmt::format("the encoder's buffer pool failed ({})",
                                         static_cast<i32>(taken))});
            }

            VE_ASSERT(CVPixelBufferGetPixelFormatType(buffer) == m_PixelFormat,
                      "The encoder's pool vended a buffer in an unexpected pixel format; the "
                      "capture's encoding fixes it.");

            IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
            id<MTLTexture> texture = surface != nullptr ? TextureFor(surface) : nil;
            if (texture == nil)
            {
                CFRelease(buffer);
                return std::unexpected(AcquireRefusal{
                    .Reason = AcquireWait::WriterFailed,
                    .Error = "the encoder's buffer could not be shared with the renderer"});
            }

            return PixelBufferHandle{
                .Texture = static_cast<void*>(texture),
                .Surface = static_cast<void*>(surface),
                .Buffer = static_cast<void*>(buffer),
            };
        }

        VoidResult AppleVideoRecorderBackend::AppendAudio(std::span<const f32> interleaved,
                                                          const u32 frames, const u64 ptsFrames)
        {
            if (m_AudioInput == nil || m_AudioFormat == nullptr || frames == 0)
            {
                return {};
            }
            if (!WaitInputReady(m_AudioInput))
            {
                return std::unexpected(Describe(m_Writer.error));
            }

            const size_t bytes = interleaved.size() * sizeof(f32);
            CMBlockBufferRef block = nullptr;
            OSStatus made = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes,
                                                               kCFAllocatorDefault, nullptr, 0,
                                                               bytes, kCMBlockBufferAssureMemoryNowFlag,
                                                               &block);
            if (made != noErr)
            {
                return std::unexpected(
                    fmt::format("a sound block could not be staged ({})", static_cast<i32>(made)));
            }
            made = CMBlockBufferReplaceDataBytes(interleaved.data(), block, 0, bytes);
            if (made != noErr)
            {
                CFRelease(block);
                return std::unexpected(
                    fmt::format("a sound block could not be filled ({})", static_cast<i32>(made)));
            }

            const CMSampleTimingInfo timing{
                .duration = CMTimeMake(1, static_cast<i32>(m_SampleRate)),
                .presentationTimeStamp =
                    CMTimeMake(static_cast<i64>(ptsFrames), static_cast<i32>(m_SampleRate)),
                .decodeTimeStamp = kCMTimeInvalid,
            };

            CMSampleBufferRef sample = nullptr;
            made = CMSampleBufferCreateReady(kCFAllocatorDefault, block, m_AudioFormat, frames, 1,
                                             &timing, 0, nullptr, &sample);
            CFRelease(block);
            if (made != noErr || sample == nullptr)
            {
                return std::unexpected(
                    fmt::format("a sound block could not be formed ({})", static_cast<i32>(made)));
            }

            const BOOL appended = [m_AudioInput appendSampleBuffer:sample];
            CFRelease(sample);
            if (!appended)
            {
                return std::unexpected(Describe(m_Writer.error));
            }
            return {};
        }

        void AppleVideoRecorderBackend::Finish(function<void()> completion)
        {
            if (m_Finishing || m_Writer == nil || m_Writer.status == AVAssetWriterStatusUnknown)
            {
                completion();
                return;
            }
            m_Finishing = true;

            [m_VideoInput markAsFinished];
            [m_AudioInput markAsFinished];

            // The completion runs on AVFoundation's own queue, possibly after this backend has been
            // destroyed, so it carries its payload on the heap and touches nothing else.
            auto* carried = new function<void()>(std::move(completion));
            [m_Writer finishWritingWithCompletionHandler:^{
                (*carried)();
                delete carried;
            }];
        }
    }

    Unique<VideoRecorderBackend> CreateVideoRecorderBackend()
    {
        return CreateUnique<AppleVideoRecorderBackend>();
    }

    bool IsVideoRecorderBackendCompiled() { return true; }
}
