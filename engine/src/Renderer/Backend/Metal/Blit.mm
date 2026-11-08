#include "Blit.h"

#include <cstring>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>

#include "Natives.h"
#include "Recording.h"
#include "TypeMapping.h"

namespace Veng::Renderer::Metal
{
    namespace
    {
        // A fullscreen triangle in Metal's own clip space (no flipped viewport here), sampling the
        // source region the parameters map the destination region's UV onto.
        constexpr const char* BlitSource = R"(
#include <metal_stdlib>
using namespace metal;

struct BlitParams
{
    float2 Offset;
    float2 Scale;
    uint Layer;
};

struct Varyings
{
    float4 Position [[position]];
    float2 Uv;
};

vertex Varyings BlitVertex(uint id [[vertex_id]])
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    Varyings out;
    out.Position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.Uv = uv;
    return out;
}

fragment float4 BlitFragment(Varyings in [[stage_in]],
                             texture2d_array<float> source [[texture(0)]],
                             sampler filter [[sampler(0)]],
                             constant BlitParams& params [[buffer(0)]])
{
    return source.sample(filter, params.Offset + in.Uv * params.Scale, params.Layer);
}
)";

        struct BlitParams
        {
            vec2 Offset;
            vec2 Scale;
            u32 Layer = 0;
        };

        uvec2 MipExtent2D(const Image& image, const u32 mip)
        {
            return {std::max(1u, image.GetExtent().x >> mip),
                    std::max(1u, image.GetExtent().y >> mip)};
        }
    }

    void BlitScaled(Context& context, const CommandBuffer& cmd, const BlitImageInfo& info)
    {
        const Image& source = *info.SourceImage;
        const Image& destination = *info.DestinationImage;
        VE_ASSERT(source.GetType() == ImageType::Type2D &&
                      destination.GetType() == ImageType::Type2D,
                  "BlitImage: a scaled blit from '{}' to '{}' takes two-dimensional images",
                  source.GetName(), destination.GetName());
        VE_ASSERT(!IsDepthStencil(source.GetFormat()) && !IsDepthStencil(destination.GetFormat()),
                  "BlitImage: a scaled blit from '{}' to '{}' takes colour images",
                  source.GetName(), destination.GetName());

        Context::Native& native = context.GetNative();
        const MTLPixelFormat format = RequireMetal(destination.GetFormat(), "BlitImage");
        const id<MTLRenderPipelineState> pipeline = native.BlitPipeline(format);
        CommandBuffer::Native& recording = cmd.GetNative();
        if (recording.BlitTable == nil)
        {
            MTL4ArgumentTableDescriptor* const descriptor =
                [[MTL4ArgumentTableDescriptor alloc] init];
            descriptor.maxBufferBindCount = 1;
            descriptor.maxTextureBindCount = 1;
            descriptor.maxSamplerStateBindCount = 1;
            descriptor.initializeBindings = YES;
            descriptor.label = @"Blit Arguments";
            NSError* error = nil;
            recording.BlitTable = [native.Device newArgumentTableWithDescriptor:descriptor
                                                                          error:&error];
            VE_ASSERT(recording.BlitTable != nil,
                      "BlitImage: failed to create its argument table: {}", Describe(error));
        }

        // A two-dimensional array view of the source level, whatever the image's own type.
        const id<MTLTexture> view = [source.GetNative().Texture
            newTextureViewWithPixelFormat:[source.GetNative().Texture pixelFormat]
                              textureType:MTLTextureType2DArray
                                   levels:NSMakeRange(info.SourceMipLevel, 1)
                                   slices:NSMakeRange(0, source.GetLayers())];
        VE_ASSERT(view != nil, "BlitImage: Metal refused a view of '{}' level {}", source.GetName(),
                  info.SourceMipLevel);

        const vec2 sourceSize(MipExtent2D(source, info.SourceMipLevel));
        const uvec2 destinationSize = MipExtent2D(destination, info.DestinationMipLevel);
        const ivec2 low = glm::min(ivec2(info.DestinationOffset), ivec2(info.DestinationExtent));
        const ivec2 high = glm::max(ivec2(info.DestinationOffset), ivec2(info.DestinationExtent));
        const bool covers = low == ivec2(0) && uvec2(high) == destinationSize;
        // A mirrored destination region is the same blit with the source region mirrored.
        vec2 sourceStart = vec2(ivec2(info.SourceOffset));
        vec2 sourceEnd = vec2(ivec2(info.SourceExtent));
        for (int axis = 0; axis < 2; ++axis)
        {
            if (info.DestinationExtent[axis] < info.DestinationOffset[axis])
            {
                std::swap(sourceStart[axis], sourceEnd[axis]);
            }
        }

        for (u32 layer = 0; layer < info.LayerCount; ++layer)
        {
            const BlitParams params{.Offset = sourceStart / sourceSize,
                                    .Scale = (sourceEnd - sourceStart) / sourceSize,
                                    .Layer = info.SourceBaseLayer + layer};
            const RingPlacement placement = recording.Ring.Allocate(sizeof(BlitParams));
            while (placement.Chunk >= recording.RingChunks.size())
            {
                recording.RingChunks.push_back(native.AcquireRingChunk(context));
            }
            const Buffer& chunk = *recording.RingChunks[placement.Chunk];
            std::memcpy(static_cast<std::byte*>(chunk.GetNative().Contents) + placement.Offset,
                        &params, sizeof(params));

            MTL4RenderPassDescriptor* const pass = [[MTL4RenderPassDescriptor alloc] init];
            MTLRenderPassColorAttachmentDescriptor* const color = pass.colorAttachments[0];
            color.texture = destination.GetNative().Texture;
            color.level = info.DestinationMipLevel;
            color.slice = info.DestinationBaseLayer + layer;
            color.loadAction = covers ? MTLLoadActionDontCare : MTLLoadActionLoad;
            color.storeAction = MTLStoreActionStore;
            pass.renderTargetWidth = destinationSize.x;
            pass.renderTargetHeight = destinationSize.y;

            [recording.BlitTable setTexture:[view gpuResourceID] atIndex:0];
            [recording.BlitTable setSamplerState:[native.BlitSampler gpuResourceID] atIndex:0];
            [recording.BlitTable setAddress:[chunk.GetNative().Buffer gpuAddress] + placement.Offset
                                    atIndex:0];

            const id<MTL4RenderCommandEncoder> encoder = Recording::RenderEncoder(cmd, pass);
            [encoder setArgumentTable:recording.BlitTable atStages:MTLRenderStageFragment];
            [encoder setRenderPipelineState:pipeline];
            [encoder setViewport:MTLViewport{.originX = static_cast<f64>(low.x),
                                             .originY = static_cast<f64>(low.y),
                                             .width = static_cast<f64>(high.x - low.x),
                                             .height = static_cast<f64>(high.y - low.y),
                                             .znear = 0.0,
                                             .zfar = 1.0}];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        native.Retire(view);
    }
}

namespace Veng::Renderer
{
    id<MTLRenderPipelineState> Context::Native::BlitPipeline(const MTLPixelFormat format)
    {
        const std::scoped_lock lock(BlitMutex);
        if (BlitLibrary == nil)
        {
            MTLCompileOptions* const options = [[MTLCompileOptions alloc] init];
            options.languageVersion = MTLLanguageVersion4_0;
            NSError* error = nil;
            BlitLibrary = [Device newLibraryWithSource:@(Metal::BlitSource)
                                               options:options
                                                 error:&error];
            VE_ASSERT(BlitLibrary != nil, "Context: the blit shader failed to compile: {}",
                      Metal::Describe(error));

            MTLSamplerDescriptor* const sampler = [[MTLSamplerDescriptor alloc] init];
            sampler.minFilter = MTLSamplerMinMagFilterLinear;
            sampler.magFilter = MTLSamplerMinMagFilterLinear;
            sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler.supportArgumentBuffers = YES;
            BlitSampler = [Device newSamplerStateWithDescriptor:sampler];
        }
        if (const auto found = BlitPipelines.find(format); found != BlitPipelines.end())
        {
            return found->second;
        }

        const auto function = [this](NSString* name)
        {
            MTL4LibraryFunctionDescriptor* const descriptor =
                [[MTL4LibraryFunctionDescriptor alloc] init];
            descriptor.library = BlitLibrary;
            descriptor.name = name;
            return descriptor;
        };
        MTL4RenderPipelineDescriptor* const descriptor =
            [[MTL4RenderPipelineDescriptor alloc] init];
        descriptor.label = @"Veng Blit";
        descriptor.vertexFunctionDescriptor = function(@"BlitVertex");
        descriptor.fragmentFunctionDescriptor = function(@"BlitFragment");
        descriptor.colorAttachments[0].pixelFormat = format;
        NSError* error = nil;
        const id<MTLRenderPipelineState> pipeline = CreateRenderPipeline(descriptor, &error);
        VE_ASSERT(pipeline != nil, "Context: the blit pipeline failed to build: {}",
                  Metal::Describe(error));
        BlitPipelines.emplace(format, pipeline);
        return pipeline;
    }
}
