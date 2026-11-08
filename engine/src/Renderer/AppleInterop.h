#pragma once

// The engine's seam to Apple's GPU objects, free of every graphics API's types: the video
// recorder and the MetalFX scalers reach Metal through it whichever backend is compiled in. The
// Vulkan backend implements it through MoltenVK's Metal-object export; the Metal backend hands out
// its own objects. The
// device (Context::GetExternalDevice) and the frame-split shared event (FrameSplitPoint) already
// reach a caller through the public Context; this adds what is per image and the release that
// follows a frame.
//
// Every object returned is +0 (unretained): a caller that keeps one retains it.
//
// On Apple the declarations below are defined by the backend compiled in. Elsewhere there is no
// platform texture to import and the inline definitions at the bottom of this header answer
// "unsupported".

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

#ifndef __APPLE__
#include <Veng/Log.h>
#endif

namespace Veng::Renderer
{
    class Context;
    class Image;
}

namespace Veng::Renderer::Apple
{
    /// @brief What an external platform texture turned out to be, read off the texture itself.
    struct ExternalTextureDescription
    {
        /// @brief The texture's width and height in texels.
        uvec2 Extent = {0, 0};
        /// @brief The engine format the texture's platform pixel format maps to.
        Renderer::Format Format = Renderer::Format::Undefined;
    };

    /// @brief Reads an external platform texture's extent and format.
    ///
    /// The import takes no extent or format from its caller because the platform implementation
    /// does not validate one: a mismatched format or extent is accepted and the channels silently
    /// swap. So both are read off the texture, through the single mapping table this and
    /// ImportExternalTexture share.
    ///
    /// The mapped formats are exactly those an external render target can be — 8-bit BGRA with an
    /// sRGB store, ten-bit packed ARGB, and half-float RGBA. Any other pixel format returns
    /// nullopt, as does a null texture or a build with no platform interop.
    /// @param texture  The platform texture handle (an @c MTLTexture on Apple).
    /// @return The texture's extent and mapped format, or nullopt when it cannot be imported.
    [[nodiscard]] VE_API optional<ExternalTextureDescription>
    DescribeExternalTexture(void* texture);

    /// @brief Imports an external platform texture as a managed engine Image.
    ///
    /// The texture's memory becomes the image's: the engine renders into it directly and never
    /// copies out of it. The image takes colour-attachment, transfer and sampled usage, and its
    /// format and extent are read off the texture (see DescribeExternalTexture).
    ///
    /// The returned image is managed — its backend image retires through the ordinary deferred path and
    /// its reference to the platform texture is released at that retire, so dropping the last Ref
    /// mid-frame is safe. Returns null when the context does not support the import, when the
    /// texture is null, or when its pixel format is unmapped; the reason is logged.
    /// @param context  The owning context; the image must not outlive it.
    /// @param texture  The platform texture handle (an @c MTLTexture on Apple).
    /// @param name     Debug name for the image.
    /// @return The imported image, or null.
    /// @pre Context::IsExternalTextureImportSupported() is true.
    [[nodiscard]] VE_API Ref<Image> ImportExternalTexture(Context& context, void* texture,
                                                          string_view name);

    /// @brief Returns the platform texture behind an engine image, as an opaque handle.
    ///
    /// The handle is unowned (+0) — the backend keeps the reference, and it is valid for as long
    /// as the image is. On the Metal backend every image's texture is returned. On the Vulkan
    /// backend it is null when the image was not created with ImageInfo::PlatformExport or the
    /// device enabled no platform-object interop. Null on a platform with none.
    /// @param context  The context the image was created on.
    /// @param image    The image to read the texture of.
    /// @return The platform texture (an @c MTLTexture on Apple), or null.
    [[nodiscard]] VE_API void* GetPlatformTexture(const Context& context, const Image& image);

    /// @brief Releases a retained platform object once the frame now recording has retired.
    ///
    /// For an object platform GPU work encoded this frame still reads: dropping it immediately
    /// could free it under that work. Outside a recording frame the release waits for the next
    /// retire point, as an engine resource dropped there does.
    /// @param context The context whose frame retire releases the object.
    /// @param object  The retained object (an Objective-C object on Apple); its one reference is
    ///                consumed.
    VE_API void ReleaseAfterFrame(const Context& context, void* object);

#ifndef __APPLE__
    inline void* GetPlatformTexture(const Context&, const Image&)
    {
        return nullptr;
    }

    inline optional<ExternalTextureDescription> DescribeExternalTexture(void*)
    {
        return std::nullopt;
    }

    inline Ref<Image> ImportExternalTexture(Context&, void*, const string_view name)
    {
        Log::Warn("Cannot import the external texture '{}': this platform has no texture interop.",
                  name);
        return nullptr;
    }

    inline void ReleaseAfterFrame(const Context&, void*) {}
#endif
}
