#pragma once

/// @file
/// @brief The one public header that exposes the Metal backend's objects, for interop only.
///
/// Everything else in include/Veng/Renderer hides the backend's objects behind the Native idiom;
/// reach for this header only when interop with Metal or a framework built on it needs the object
/// itself. It exists only in a build whose GPU backend is Metal (VE_GPU_BACKEND), and refuses to
/// compile in any other. It is the sibling of Native.h, under the same doctrine: `GetNative()
/// const` returns a mutable reference, and a const resource is one whose identity, not whose GPU
/// state, does not change.
///
/// Every accessor returns its object unretained (+0), as an opaque pointer so a C++ translation
/// unit can carry it; the backend keeps the reference, valid for as long as the engine object is.
/// An Objective-C translation unit gets typed wrappers too, which bridge-cast the same pointer.

#ifndef VE_GPU_BACKEND_METAL
#error                                                                                             \
    "Veng/Renderer/NativeMetal.h exposes Metal objects and compiles only in a Metal-backend build"
#endif

#include <Veng/Export.h>

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class ComputePipeline;
    class Context;
    class Fence;
    class Image;
    class ImageView;
    class Sampler;
    class ShaderModule;
    class TimelineSemaphore;

    /// @brief The context's device (an @c MTLDevice).
    [[nodiscard]] VE_API void* GetMetalDevice(const Context& context);

    /// @brief The context's one command queue (an @c MTL4CommandQueue).
    [[nodiscard]] VE_API void* GetMetalCommandQueue(const Context& context);

    /// @brief The buffer (an @c MTLBuffer).
    [[nodiscard]] VE_API void* GetMetalBuffer(const Buffer& buffer);

    /// @brief The image's texture (an @c MTLTexture).
    [[nodiscard]] VE_API void* GetMetalTexture(const Image& image);

    /// @brief The view's texture view (an @c MTLTexture).
    [[nodiscard]] VE_API void* GetMetalTexture(const ImageView& view);

    /// @brief The sampler state (an @c MTLSamplerState).
    [[nodiscard]] VE_API void* GetMetalSamplerState(const Sampler& sampler);

    /// @brief The module's library (an @c MTLLibrary).
    [[nodiscard]] VE_API void* GetMetalLibrary(const ShaderModule& module);

    /// @brief The pipeline state (an @c MTLComputePipelineState).
    [[nodiscard]] VE_API void* GetMetalComputePipelineState(const ComputePipeline& pipeline);

    /// @brief The command buffer recorded into (an @c MTL4CommandBuffer).
    [[nodiscard]] VE_API void* GetMetalCommandBuffer(const CommandBuffer& commandBuffer);

    /// @brief The fence's shared event (an @c MTLSharedEvent).
    [[nodiscard]] VE_API void* GetMetalSharedEvent(const Fence& fence);

    /// @brief The timeline's shared event (an @c MTLSharedEvent).
    [[nodiscard]] VE_API void* GetMetalSharedEvent(const TimelineSemaphore& timeline);
}

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Veng::Renderer
{
    /// @brief The context's device, typed.
    inline id<MTLDevice> GetMTLDevice(const Context& context)
    {
        return (__bridge id<MTLDevice>)GetMetalDevice(context);
    }

    /// @brief The context's command queue, typed.
    inline id<MTL4CommandQueue> GetMTL4CommandQueue(const Context& context)
    {
        return (__bridge id<MTL4CommandQueue>)GetMetalCommandQueue(context);
    }

    /// @brief The buffer, typed.
    inline id<MTLBuffer> GetMTLBuffer(const Buffer& buffer)
    {
        return (__bridge id<MTLBuffer>)GetMetalBuffer(buffer);
    }

    /// @brief The image's texture, typed.
    inline id<MTLTexture> GetMTLTexture(const Image& image)
    {
        return (__bridge id<MTLTexture>)GetMetalTexture(image);
    }

    /// @brief The view's texture view, typed.
    inline id<MTLTexture> GetMTLTexture(const ImageView& view)
    {
        return (__bridge id<MTLTexture>)GetMetalTexture(view);
    }
}
#endif
