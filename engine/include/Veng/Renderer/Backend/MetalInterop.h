#pragma once

// Importing an externally-owned platform texture as an engine Image. Internal to the backend, but
// deliberately free of both Vulkan and Metal types: a consumer holds the platform texture as an
// opaque handle and never sees either API.
//
// On Apple the declarations below are defined in MetalInterop.mm, the one translation unit that
// names Metal. Elsewhere there is no platform texture to import and the inline definitions at the
// bottom of this header answer "unsupported".

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

namespace Veng::Renderer::Backend
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
    [[nodiscard]] optional<ExternalTextureDescription> DescribeExternalTexture(void* texture);

    /// @brief Imports an external platform texture as a managed engine Image.
    ///
    /// The texture's memory becomes the image's: the engine renders into it directly and never
    /// copies out of it. The image takes colour-attachment, transfer and sampled usage, and its
    /// format and extent are read off the texture (see DescribeExternalTexture).
    ///
    /// The returned image is managed — its VkImage retires through the ordinary deferred path and
    /// its reference to the platform texture is released at that retire, so dropping the last Ref
    /// mid-frame is safe. Returns null when the context does not support the import, when the
    /// texture is null, or when its pixel format is unmapped; the reason is logged.
    /// @param context  The owning context; the image must not outlive it.
    /// @param texture  The platform texture handle (an @c MTLTexture on Apple).
    /// @param name     Debug name for the image.
    /// @return The imported image, or null.
    /// @pre Context::IsExternalTextureImportSupported() is true.
    [[nodiscard]] Ref<Image> ImportExternalTexture(Context& context, void* texture,
                                                   string_view name);

#ifndef __APPLE__
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
#endif
}
