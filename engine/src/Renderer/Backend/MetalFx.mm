#include "MetalFx.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/MetalInterop.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer::Backend
{
    namespace
    {
        /// @brief Maps the engine formats the scalers read and write to their Metal pixel formats.
        MTLPixelFormat PixelFormatFor(const Format format)
        {
            switch (format)
            {
            case Format::RGBA16Sfloat:
                return MTLPixelFormatRGBA16Float;
            case Format::RG16Sfloat:
                return MTLPixelFormatRG16Float;
            case Format::D32Sfloat:
                return MTLPixelFormatDepth32Float;
            case Format::R16Sfloat:
                return MTLPixelFormatR16Float;
            case Format::R8Unorm:
                return MTLPixelFormatR8Unorm;
            default:
                VE_ASSERT(false, "MetalFX: no Metal pixel format mapped for engine format {}",
                          static_cast<u32>(format));
                return MTLPixelFormatInvalid;
            }
        }

        /// @brief The exported texture of an image a scaler reads or writes, checked for use.
        id<MTLTexture> TextureFor(const Context& context, const Image& image,
                                  const MTLTextureUsage required, const char* role)
        {
            id<MTLTexture> texture = (id<MTLTexture>)GetPlatformTexture(context, image);
            VE_ASSERT(texture != nil, "MetalFX: the {} image '{}' exports no texture", role,
                      image.GetName());
            VE_ASSERT(([texture usage] & required) == required,
                      "MetalFX: the {} image '{}' lacks the texture usage the scaler requires "
                      "(has {:#x}, needs {:#x})",
                      role, image.GetName(), static_cast<u64>([texture usage]),
                      static_cast<u64>(required));
            return texture;
        }

        /// @brief Schedules a scaler's work between two segments of the recording frame.
        ///
        /// The command buffer is committed to the scaler's own queue: the frame segment it waits on
        /// may not have been encoded yet, and a wait on the frame's own queue would hold that
        /// queue against work submitted behind it.
        template <typename EncodeFn>
        void EncodeBetweenSegments(Context& context, void* queue, EncodeFn&& encode)
        {
            context.SplitFrame(
                [&](const FrameSplitPoint& point)
                {
                    @autoreleasepool
                    {
                        id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>)queue commandBuffer];
                        [commandBuffer setLabel:@"MetalFX"];
                        id<MTLSharedEvent> event = (id<MTLSharedEvent>)point.SharedEvent;
                        [commandBuffer encodeWaitForEvent:event value:point.WaitValue];
                        encode(commandBuffer);
                        [commandBuffer encodeSignalEvent:event value:point.SignalValue];
                        [commandBuffer commit];
                    }
                });
        }

        /// @brief Defers releasing a scaler and its queue until the frame that used them retires.
        void RetireObjects(Context& context, void* scaler, void* queue)
        {
            context.GetNative().Retire(
                [scaler, queue]
                {
                    [(id)scaler release];
                    [(id)queue release];
                });
        }
    }

    MetalFxSupport QueryMetalFxSupport(const Context& context)
    {
        id<MTLDevice> device = (id<MTLDevice>)context.GetExternalDevice();
        if (device == nil || !context.IsFrameSplitSupported())
        {
            return {};
        }

        // MetalFX is weak-linked, so on a system without it the class is nil and every message
        // to it answers NO.
        MetalFxSupport support{
            .Temporal = [MTLFXTemporalScalerDescriptor supportsDevice:device] == YES,
            .Spatial = [MTLFXSpatialScalerDescriptor supportsDevice:device] == YES,
        };
        if (support.Temporal)
        {
            support.TemporalMinScale =
                [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:device];
            support.TemporalMaxScale =
                [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:device];
        }
        return support;
    }

    Unique<MetalFxTemporalScaler> MetalFxTemporalScaler::Create(Context& context,
                                                                const MetalFxTemporalInfo& info)
    {
        id<MTLDevice> device = (id<MTLDevice>)context.GetExternalDevice();
        if (device == nil || !context.IsFrameSplitSupported())
        {
            Log::Warn("MetalFX temporal scaler: this device has no platform-object interop");
            return nullptr;
        }

        const MetalFxSupport support = QueryMetalFxSupport(context);
        // The ratio the full input allocation reconstructs at, which a frame rendering its whole
        // allocation encodes; a dynamic-resolution sub-rect only raises it, up to MaxScale.
        const f32 allocationScale =
            static_cast<f32>(info.OutputExtent.x) / static_cast<f32>(info.InputExtent.x);
        if (!support.Temporal || allocationScale < support.TemporalMinScale - 1e-4f ||
            info.MaxScale > support.TemporalMaxScale + 1e-4f)
        {
            Log::Warn("MetalFX temporal scaler: {}x{} -> {}x{} (content scale {:.3f}..{:.3f}) is "
                      "outside what the device supports ({:.3f}..{:.3f})",
                      info.InputExtent.x, info.InputExtent.y, info.OutputExtent.x,
                      info.OutputExtent.y, allocationScale, info.MaxScale,
                      support.TemporalMinScale, support.TemporalMaxScale);
            return nullptr;
        }

        MTLFXTemporalScalerDescriptor* descriptor = [[MTLFXTemporalScalerDescriptor alloc] init];
        descriptor.colorTextureFormat = PixelFormatFor(info.ColorFormat);
        descriptor.depthTextureFormat = PixelFormatFor(info.DepthFormat);
        descriptor.motionTextureFormat = PixelFormatFor(info.MotionFormat);
        descriptor.outputTextureFormat = PixelFormatFor(info.OutputFormat);
        descriptor.inputWidth = info.InputExtent.x;
        descriptor.inputHeight = info.InputExtent.y;
        descriptor.outputWidth = info.OutputExtent.x;
        descriptor.outputHeight = info.OutputExtent.y;
        // The input is linear HDR at the scene's own scale, and the scaler weighs it at the exposure
        // the tonemap applies (Encode's exposure texture) and returns linear HDR. Metering its own
        // exposure off a mostly black frame — space — it picks one far higher, saturates bright
        // highlights in its internal tone curve, and shifts their hue toward red.
        descriptor.autoExposureEnabled = NO;
        // A dynamic-resolution frame renders a top-left sub-rect of the input allocation.
        descriptor.inputContentPropertiesEnabled = YES;
        descriptor.inputContentMinScale = allocationScale;
        descriptor.inputContentMaxScale = std::max(info.MaxScale, allocationScale);
        // Created where a resize already stalls the frame; an asynchronous initialization would
        // instead pass frames through at low quality until it finished.
        descriptor.requiresSynchronousInitialization = YES;
        // The reactive mask says where the input holds content whose motion the motion texture
        // does not carry; the API arrived in macOS 14.4.
        bool reactive = false;
        if (info.ReactiveFormat != Format::Undefined)
        {
            if (@available(macOS 14.4, iOS 17.4, *))
            {
                descriptor.reactiveMaskTextureEnabled = YES;
                descriptor.reactiveMaskTextureFormat = PixelFormatFor(info.ReactiveFormat);
                reactive = true;
            }
        }

        id<MTLFXTemporalScaler> scaler = [descriptor newTemporalScalerWithDevice:device];
        [descriptor release];
        if (scaler == nil)
        {
            Log::Warn("MetalFX temporal scaler: the device refused {}x{} -> {}x{}",
                      info.InputExtent.x, info.InputExtent.y, info.OutputExtent.x,
                      info.OutputExtent.y);
            return nullptr;
        }
        scaler.depthReversed = info.DepthReversed ? YES : NO;

        id<MTLCommandQueue> queue = [device newCommandQueue];
        [queue setLabel:@"MetalFX Temporal"];

        // What the scaler was created with, so Encode binds a reactive mask only to one that takes it.
        MetalFxTemporalInfo created = info;
        if (!reactive)
        {
            created.ReactiveFormat = Format::Undefined;
        }

        Unique<MetalFxTemporalScaler> made(
            new MetalFxTemporalScaler(context, created, (void*)scaler, (void*)queue));
        MTLTextureDescriptor* exposure =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        exposure.storageMode = MTLStorageModeShared;
        exposure.usage = MTLTextureUsageShaderRead;
        for (u32 slot = 0; slot < context.GetMaxFramesInFlight(); ++slot)
        {
            id<MTLTexture> texture = [device newTextureWithDescriptor:exposure];
            [texture setLabel:@"MetalFX Exposure"];
            made->m_Exposure.push_back((void*)texture);
        }
        return made;
    }

    MetalFxTemporalScaler::MetalFxTemporalScaler(Context& context, const MetalFxTemporalInfo& info,
                                                 void* scaler, void* queue)
        : m_Context(context), m_Info(info), m_Scaler(scaler), m_Queue(queue)
    {
    }

    MetalFxTemporalScaler::~MetalFxTemporalScaler()
    {
        RetireObjects(m_Context, m_Scaler, m_Queue);
        for (void* texture : m_Exposure)
        {
            m_Context.GetNative().Retire([texture] { [(id)texture release]; });
        }
    }

    void MetalFxTemporalScaler::Encode(const MetalFxTemporalFrame& frame)
    {
        VE_ASSERT(frame.Color && frame.Depth && frame.Motion && frame.Output,
                  "MetalFX temporal scaler: every input and the output must be set");

        id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)m_Scaler;
        scaler.colorTexture = TextureFor(m_Context, *frame.Color, scaler.colorTextureUsage, "colour");
        scaler.depthTexture = TextureFor(m_Context, *frame.Depth, scaler.depthTextureUsage, "depth");
        scaler.motionTexture =
            TextureFor(m_Context, *frame.Motion, scaler.motionTextureUsage, "motion");
        scaler.outputTexture =
            TextureFor(m_Context, *frame.Output, scaler.outputTextureUsage, "output");
        scaler.inputContentWidth = std::clamp<u32>(frame.ContentExtent.x, 1, m_Info.InputExtent.x);
        scaler.inputContentHeight = std::clamp<u32>(frame.ContentExtent.y, 1, m_Info.InputExtent.y);
        scaler.jitterOffsetX = frame.JitterPixels.x;
        scaler.jitterOffsetY = frame.JitterPixels.y;
        scaler.motionVectorScaleX = frame.MotionScale.x;
        scaler.motionVectorScaleY = frame.MotionScale.y;
        scaler.reset = frame.Reset ? YES : NO;
        id<MTLTexture> exposure = (id<MTLTexture>)
            m_Exposure[m_Context.GetCurrentFrameInFlight() % static_cast<u32>(m_Exposure.size())];
        const f32 value = frame.Exposure;
        [exposure replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                    mipmapLevel:0
                      withBytes:&value
                    bytesPerRow:sizeof(f32)];
        scaler.exposureTexture = exposure;
        if (m_Info.ReactiveFormat != Format::Undefined && frame.Reactive != nullptr)
        {
            if (@available(macOS 14.4, iOS 17.4, *))
            {
                scaler.reactiveMaskTexture = TextureFor(m_Context, *frame.Reactive,
                                                        scaler.reactiveTextureUsage, "reactive");
            }
        }

        EncodeBetweenSegments(m_Context, m_Queue, [scaler](id<MTLCommandBuffer> commandBuffer)
                              { [scaler encodeToCommandBuffer:commandBuffer]; });
    }

    Unique<MetalFxSpatialScaler> MetalFxSpatialScaler::Create(Context& context,
                                                              const MetalFxSpatialInfo& info)
    {
        id<MTLDevice> device = (id<MTLDevice>)context.GetExternalDevice();
        if (device == nil || !context.IsFrameSplitSupported() ||
            !QueryMetalFxSupport(context).Spatial)
        {
            Log::Warn("MetalFX spatial scaler: unsupported on this device");
            return nullptr;
        }

        MTLFXSpatialScalerDescriptor* descriptor = [[MTLFXSpatialScalerDescriptor alloc] init];
        descriptor.colorTextureFormat = PixelFormatFor(info.ColorFormat);
        descriptor.outputTextureFormat = PixelFormatFor(info.OutputFormat);
        descriptor.inputWidth = info.InputExtent.x;
        descriptor.inputHeight = info.InputExtent.y;
        descriptor.outputWidth = info.OutputExtent.x;
        descriptor.outputHeight = info.OutputExtent.y;
        // The scene colour ahead of the tonemap: linear, and unbounded above one.
        descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeHDR;

        id<MTLFXSpatialScaler> scaler = [descriptor newSpatialScalerWithDevice:device];
        [descriptor release];
        if (scaler == nil)
        {
            Log::Warn("MetalFX spatial scaler: the device refused {}x{} -> {}x{}",
                      info.InputExtent.x, info.InputExtent.y, info.OutputExtent.x,
                      info.OutputExtent.y);
            return nullptr;
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        [queue setLabel:@"MetalFX Spatial"];

        return Unique<MetalFxSpatialScaler>(
            new MetalFxSpatialScaler(context, info, (void*)scaler, (void*)queue));
    }

    MetalFxSpatialScaler::MetalFxSpatialScaler(Context& context, const MetalFxSpatialInfo& info,
                                               void* scaler, void* queue)
        : m_Context(context), m_Info(info), m_Scaler(scaler), m_Queue(queue)
    {
    }

    MetalFxSpatialScaler::~MetalFxSpatialScaler()
    {
        RetireObjects(m_Context, m_Scaler, m_Queue);
    }

    void MetalFxSpatialScaler::Encode(const MetalFxSpatialFrame& frame)
    {
        VE_ASSERT(frame.Color && frame.Output,
                  "MetalFX spatial scaler: the input and the output must be set");

        id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)m_Scaler;
        scaler.colorTexture = TextureFor(m_Context, *frame.Color, scaler.colorTextureUsage, "colour");
        scaler.outputTexture =
            TextureFor(m_Context, *frame.Output, scaler.outputTextureUsage, "output");
        scaler.inputContentWidth = std::clamp<u32>(frame.ContentExtent.x, 1, m_Info.InputExtent.x);
        scaler.inputContentHeight = std::clamp<u32>(frame.ContentExtent.y, 1, m_Info.InputExtent.y);

        EncodeBetweenSegments(m_Context, m_Queue, [scaler](id<MTLCommandBuffer> commandBuffer)
                              { [scaler encodeToCommandBuffer:commandBuffer]; });
    }
}
