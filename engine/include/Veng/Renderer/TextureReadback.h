#pragma once

#include <Veng/Export.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;

    /// @brief One mip level of an image read back to the host as linear float RGBA.
    struct TexelReadback
    {
        /// @brief The level's extent in texels.
        uvec2 Extent{0};
        /// @brief The array layers read: six for a cube, one for a plain 2D image.
        u32 Layers = 1;
        /// @brief The format the image stores, before decoding.
        Format SourceFormat = Format::Undefined;
        /// @brief Every texel, layer-major then row-major, top row first.
        ///
        /// Decoded as a shader fetch decodes it: an sRGB format is linearized, a channel the
        /// format lacks reads 0 (alpha 1), and a compressed block is expanded.
        vector<vec4> Texels;
    };

    /// @brief Whether ReadbackTexels can decode an image stored in @p format.
    ///
    /// True for every sampleable colour format — normalized, sRGB, float and block-compressed — and
    /// false for depth, stencil and integer formats, which a float fetch cannot read.
    /// @param format The stored format.
    /// @return Whether the format reads back.
    [[nodiscard]] VE_API bool IsTexelReadable(Format format);

    /// @brief Whether @p format stores sRGB-encoded colour that a fetch linearizes.
    /// @param format The stored format.
    /// @return True for the sRGB formats.
    [[nodiscard]] VE_API bool IsSrgbFormat(Format format);

    /// @brief Reads one mip level of a 2D or layered image back to the host, blocking until done.
    ///
    /// Fetches every texel through a compute pass, so the image needs only Sampled usage — a cooked
    /// texture, a baked cube or a render target all read back the same way — and any readable
    /// format decodes to float. Runs through ImmediateCommands and waits for it, so it is for
    /// tooling and diagnostics, never a frame path. The image is left prepared for sampling.
    /// @param context  The render context the image belongs to.
    /// @param assets   The asset manager the core readback shader loads through.
    /// @param image    The image to read: a 2D image, an array or a cube, with Sampled usage.
    /// @param mipLevel The mip level to read.
    /// @return The decoded level, or the reason the image cannot be read.
    [[nodiscard]] VE_API Result<TexelReadback> ReadbackTexels(Context& context,
                                                              AssetManager& assets,
                                                              const Ref<Image>& image,
                                                              u32 mipLevel = 0);
}
