#include "Blit.h"

#include <cstring>
#include <string>

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

        // The argument table the backend's own render passes bind, created at first use.
        void EnsureBlitTable(const Context::Native& native, CommandBuffer::Native& recording)
        {
            if (recording.BlitTable != nil)
            {
                return;
            }
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
                      "CommandBuffer: failed to create the blit argument table: {}",
                      Describe(error));
        }

        // Copies @p bytes into the recording's ring and returns their GPU address.
        u64 RingCopy(Context& context, CommandBuffer::Native& recording, const void* bytes,
                     const u32 size)
        {
            const RingPlacement placement = recording.Ring.Allocate(size);
            while (placement.Chunk >= recording.RingChunks.size())
            {
                recording.RingChunks.push_back(context.GetNative().AcquireRingChunk(context));
            }
            const Buffer& chunk = *recording.RingChunks[placement.Chunk];
            std::memcpy(static_cast<std::byte*>(chunk.GetNative().Contents) + placement.Offset,
                        bytes, size);
            return [chunk.GetNative().Buffer gpuAddress] + placement.Offset;
        }

        // The area clear's parameters: each colour slot's value, then the depth.
        struct AreaClearParams
        {
            std::array<vec4, MaxAreaClearAttachments> Colors;
            f32 Depth = 0.0f;
        };

        // Whether a colour format stores unsigned integers, which a fragment writes as uint4.
        bool IsUnsignedIntegerFormat(const MTLPixelFormat format)
        {
            switch (format)
            {
            case MTLPixelFormatR32Uint:
            case MTLPixelFormatRGBA16Uint:
            case MTLPixelFormatR8Uint:
            case MTLPixelFormatR16Uint:
            case MTLPixelFormatRG32Uint:
            case MTLPixelFormatRGBA32Uint:
                return true;
            default:
                return false;
            }
        }

        // The area clear's shaders for one clear: a triangle over the viewport, and a fragment
        // writing each cleared slot's value (as uint4 to an integer format) and the depth.
        std::string AreaClearSource(const AreaClear& clear)
        {
            std::string outputs;
            std::string writes;
            for (u32 slot = 0; slot < MaxAreaClearAttachments; ++slot)
            {
                if ((clear.ColorMask & (1u << slot)) == 0)
                {
                    continue;
                }
                const bool integer = IsUnsignedIntegerFormat(clear.ColorFormats[slot]);
                const std::string index = std::to_string(slot);
                outputs += std::string(integer ? "    uint4" : "    float4") + " Color" + index +
                           " [[color(" + index + ")]];\n";
                writes += "    out.Color" + index + " = " +
                          (integer ? "uint4(params.Colors[" + index + "])"
                                   : "params.Colors[" + index + "]") +
                          ";\n";
            }
            if (clear.ClearsDepth)
            {
                outputs += "    float Depth [[depth(any)]];\n";
                writes += "    out.Depth = params.Depth;\n";
            }
            return "#include <metal_stdlib>\nusing namespace metal;\n"
                   "struct Params { float4 Colors[8]; float Depth; };\n"
                   "struct Output {\n" +
                   outputs +
                   "};\n"
                   "vertex float4 ClearVertex(uint id [[vertex_id]]) {\n"
                   "    const float2 uv = float2((id << 1) & 2, id & 2);\n"
                   "    return float4(uv * 2.0 - 1.0, 0.0, 1.0);\n}\n"
                   "fragment Output ClearFragment(constant Params& params [[buffer(0)]]) {\n"
                   "    Output out;\n" +
                   writes + "    return out;\n}\n";
        }

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
        EnsureBlitTable(native, recording);

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
            const u64 paramsAddress = RingCopy(context, recording, &params, sizeof(params));

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
            [recording.BlitTable setAddress:paramsAddress atIndex:0];

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

    void ClearRenderArea(Context& context, const CommandBuffer& cmd, const AreaClear& clear)
    {
        CommandBuffer::Native& recording = cmd.GetNative();
        VE_ASSERT(recording.Render != nil, "ClearRenderArea: no render pass is open");
        Context::Native& native = context.GetNative();
        EnsureBlitTable(native, recording);

        AreaClearParams params{.Colors = clear.Colors, .Depth = clear.Depth};
        [recording.BlitTable setAddress:RingCopy(context, recording, &params, sizeof(params))
                                atIndex:0];

        const id<MTL4RenderCommandEncoder> encoder = recording.Render;
        [encoder setArgumentTable:recording.BlitTable atStages:MTLRenderStageFragment];
        [encoder setRenderPipelineState:native.ClearPipeline(clear)];
        [encoder setDepthStencilState:native.ClearDepthState(clear.ClearsDepth)];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setTriangleFillMode:MTLTriangleFillModeFill];
        [encoder setDepthClipMode:MTLDepthClipModeClip];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder setArgumentTable:recording.FragmentTable atStages:MTLRenderStageFragment];
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

    id<MTLRenderPipelineState> Context::Native::ClearPipeline(const Metal::AreaClear& clear)
    {
        std::string key = std::to_string(clear.ColorMask) + "/" +
                          std::to_string(clear.ClearsDepth ? 1 : 0) + "/" +
                          std::to_string(clear.SampleCount);
        for (const MTLPixelFormat format : clear.ColorFormats)
        {
            key += "/" + std::to_string(format);
        }

        const std::scoped_lock lock(BlitMutex);
        if (const auto found = ClearPipelines.find(key); found != ClearPipelines.end())
        {
            return found->second;
        }

        MTLCompileOptions* const options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion4_0;
        NSError* error = nil;
        const id<MTLLibrary> library =
            [Device newLibraryWithSource:@(Metal::AreaClearSource(clear).c_str())
                                 options:options
                                   error:&error];
        VE_ASSERT(library != nil, "Context: the area clear shader failed to compile: {}",
                  Metal::Describe(error));

        MTL4RenderPipelineDescriptor* const descriptor =
            [[MTL4RenderPipelineDescriptor alloc] init];
        descriptor.label = @"Veng Area Clear";
        MTL4LibraryFunctionDescriptor* const vertex = [[MTL4LibraryFunctionDescriptor alloc] init];
        vertex.library = library;
        vertex.name = @"ClearVertex";
        MTL4LibraryFunctionDescriptor* const fragment =
            [[MTL4LibraryFunctionDescriptor alloc] init];
        fragment.library = library;
        fragment.name = @"ClearFragment";
        descriptor.vertexFunctionDescriptor = vertex;
        descriptor.fragmentFunctionDescriptor = fragment;
        descriptor.rasterSampleCount = clear.SampleCount;
        for (u32 slot = 0; slot < Metal::MaxAreaClearAttachments; ++slot)
        {
            descriptor.colorAttachments[slot].pixelFormat = clear.ColorFormats[slot];
            descriptor.colorAttachments[slot].writeMask = (clear.ColorMask & (1u << slot)) != 0
                                                              ? MTLColorWriteMaskAll
                                                              : MTLColorWriteMaskNone;
        }
        const id<MTLRenderPipelineState> pipeline = CreateRenderPipeline(descriptor, &error);
        VE_ASSERT(pipeline != nil, "Context: the area clear pipeline failed to build: {}",
                  Metal::Describe(error));
        ClearPipelines.emplace(std::move(key), pipeline);
        return pipeline;
    }

    id<MTLDepthStencilState> Context::Native::ClearDepthState(const bool writesDepth)
    {
        const std::scoped_lock lock(BlitMutex);
        __strong id<MTLDepthStencilState>& state = writesDepth ? ClearDepthWrite : ClearDepthKeep;
        if (state == nil)
        {
            MTLDepthStencilDescriptor* const descriptor = [[MTLDepthStencilDescriptor alloc] init];
            descriptor.depthCompareFunction = MTLCompareFunctionAlways;
            descriptor.depthWriteEnabled = writesDepth;
            state = [Device newDepthStencilStateWithDescriptor:descriptor];
        }
        return state;
    }
}
