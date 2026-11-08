// The Metal backend's side of the Apple interop seam: its objects are already Metal, so the device
// and a texture are handed out directly, an external texture is wrapped as an image, and platform
// work encodes straight into the recording command buffer.

#include "../../AppleInterop.h"

#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>

#include "BarrierMapping.h"
#include "Natives.h"
#include "Recording.h"

namespace Veng::Renderer::Apple
{
    namespace
    {
        /// @brief Maps a Metal pixel format to the engine format an imported image takes.
        ///
        /// The closed set an external render target can be: an 8-bit BGRA whose store applies the
        /// sRGB transfer, the ten-bit packed ARGB word, and half-float RGBA. Plain BGRA8Unorm is
        /// absent: a consumer wanting 8-bit standard range wants the sRGB store, and accepting the
        /// unorm twin would write linear values under an sRGB tag.
        optional<Renderer::Format> EngineFormatFor(const MTLPixelFormat format)
        {
            switch (format)
            {
            case MTLPixelFormatBGRA8Unorm_sRGB:
                return Renderer::Format::BGRA8Srgb;
            case MTLPixelFormatBGR10A2Unorm:
                return Renderer::Format::A2R10G10B10Unorm;
            case MTLPixelFormatRGBA16Float:
                return Renderer::Format::RGBA16Sfloat;
            default:
                return std::nullopt;
            }
        }
    }

    optional<ExternalTextureDescription> DescribeExternalTexture(void* texture)
    {
        const auto mtlTexture = (__bridge id<MTLTexture>)texture;
        if (mtlTexture == nil)
        {
            return std::nullopt;
        }
        const optional<Renderer::Format> format = EngineFormatFor([mtlTexture pixelFormat]);
        if (!format)
        {
            return std::nullopt;
        }
        return ExternalTextureDescription{
            .Extent =
                uvec2{static_cast<u32>([mtlTexture width]), static_cast<u32>([mtlTexture height])},
            .Format = *format,
        };
    }

    void* GetPlatformTexture(const Context& /*context*/, const Image& image)
    {
        return (__bridge void*)image.GetNative().Texture;
    }

    Ref<Image> ImportExternalTexture(Context& context, void* texture, const string_view name)
    {
        const optional<ExternalTextureDescription> description = DescribeExternalTexture(texture);
        if (!description)
        {
            Log::Warn("Cannot import the external texture '{}': it is null or carries a pixel "
                      "format no engine format maps.",
                      name);
            return nullptr;
        }

        const ImageInfo info{
            .Name = string(name),
            .Extent = uvec3{description->Extent.x, description->Extent.y, 1u},
            .MipLevels = 1,
            .Layers = 1,
            .Format = description->Format,
            .Type = ImageType::Type2D,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc |
                     ImageUsage::TransferDst | ImageUsage::Sampled,
        };

        // The image holds its own reference to the texture, and the texture joins the residency
        // set for as long as the image lives; both go in the retire the image's destructor defers.
        const auto mtlTexture = (__bridge id<MTLTexture>)texture;
        Context::Native& native = context.GetNative();
        native.MakeResident(mtlTexture);
        auto imported = CreateUnique<Image::Native>();
        imported->Texture = mtlTexture;
        imported->Teardown = [&native, mtlTexture] { native.Evict(mtlTexture); };
        return Image::CreateImported(context, info, std::move(imported));
    }

    void ReleaseAfterFrame(const Context& context, void* object)
    {
        context.GetNative().Retire(function<void()>([object] { CFRelease(object); }));
    }

    void EncodeIntoRecording(const Context& owner, const CommandBuffer& cmd,
                             const function<void(void* commandBuffer, void* fence)>& encode)
    {
        using Metal::MetalStage;
        CommandBuffer::Native& native = cmd.GetNative();
        Context::Native& context = owner.GetNative();
        if (context.InteropFence == nil)
        {
            context.InteropFence = [context.Device newFence];
            [context.InteropFence setLabel:@"Platform Work Fence"];
        }

        // The platform's own encoders apply none of the recording's barriers, so an encoder that
        // waits on everything earlier updates the fence the work waits instead.
        Metal::Recording::Barrier(cmd, MetalStage::All, Metal::ComputeEncoderStages);
        const id<MTL4ComputeCommandEncoder> encoder = Metal::Recording::ComputeEncoder(cmd);
        [encoder updateFence:context.InteropFence
            afterEncoderStages:static_cast<MTLStages>(Metal::ComputeEncoderStages)];
        Metal::Recording::EndEncoder(cmd);

        encode((__bridge void*)native.CommandBuffer, (__bridge void*)context.InteropFence);

        // What follows waits on every stage of the work, whichever encoders it opened.
        native.PendingAfter |= MetalStage::All;
        native.PendingBefore |= MetalStage::All;
    }

    void* GetPipelineCompiler(const Context& context)
    {
        return (__bridge void*)context.GetNative().Compiler;
    }

    void MakeResident(const Context& context, void* allocation)
    {
        context.GetNative().MakeResident((__bridge id<MTLAllocation>)allocation);
    }

    void ReleaseResidentAfterFrame(const Context& context, void* allocation)
    {
        Context::Native& native = context.GetNative();
        native.Retire(function<void()>(
            [&native, allocation]
            {
                native.Evict((__bridge id<MTLAllocation>)allocation);
                CFRelease(allocation);
            }));
    }
}
