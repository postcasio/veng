#pragma once

// The byte layout a set of generated-texture targets takes inside one derived-data cache entry,
// separated from the service so the encoding is a pure function of its arguments and is testable
// with no ICD (the FrameTopology / GeneratedTextureQueue precedent). It creates nothing, reads no
// file, and touches no device: bytes in, bytes out.

#include <span>

#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief One target's image shape, as stored beside its texels.
    ///
    /// Everything a restore needs to know that the payload alone does not say. A stored shape that
    /// differs in any field from the target waiting for it makes the whole entry unusable: the
    /// texels would be uploaded into an image they do not describe.
    struct GeneratedTextureBlobShape
    {
        /// @brief The texel format.
        Format TexelFormat = Format::Undefined;
        /// @brief The image's dimensionality.
        ImageType Type = ImageType::Type2D;
        /// @brief Mip 0's width, height, and depth in texels.
        uvec3 Extent = {1, 1, 1};
        /// @brief The array layer count.
        u32 Layers = 1;
        /// @brief The mip level count.
        u32 MipLevels = 1;

        /// @brief Equality over every field; a restore requires an exact match.
        [[nodiscard]] bool operator==(const GeneratedTextureBlobShape&) const = default;
    };

    /// @brief A decoded entry: the shapes it holds and the texels behind them.
    struct GeneratedTextureBlob
    {
        /// @brief One shape per target, in the order the job declared them.
        vector<GeneratedTextureBlobShape> Shapes;
        /// @brief Every target's texels, concatenated in shape order.
        ///
        /// Within one target the levels run mip-major, layer-minor — mip 0's layers first, then
        /// mip 1's — which is both the order the readbacks deliver them in and the order a
        /// per-mip, all-layers buffer-to-image copy expects.
        vector<u8> Texels;
    };

    /// @brief Bytes one array layer of one mip level of a shape occupies.
    /// @param shape     The image shape.
    /// @param mipLevel  The mip level.
    /// @return The tightly-packed byte size, or 0 for a format with no known size.
    [[nodiscard]] usize GeneratedTextureLayerBytes(const GeneratedTextureBlobShape& shape,
                                                   u32 mipLevel);

    /// @brief Bytes a shape's whole mip chain across every layer occupies.
    /// @param shape  The image shape.
    /// @return The tightly-packed byte size, or 0 for a format with no known size.
    [[nodiscard]] usize GeneratedTextureShapeBytes(const GeneratedTextureBlobShape& shape);

    /// @brief Byte offset of one mip level's first layer within a shape's texels.
    /// @param shape     The image shape.
    /// @param mipLevel  The mip level.
    /// @return The offset from the start of that shape's texels.
    [[nodiscard]] usize GeneratedTextureMipOffset(const GeneratedTextureBlobShape& shape,
                                                  u32 mipLevel);

    /// @brief Encodes shapes and texels into one cache payload.
    ///
    /// Returns an empty buffer when the texels do not add up to what the shapes describe, so a
    /// caller that assembled a short readback set stores nothing rather than an entry that decodes
    /// into the wrong texels.
    /// @param blob  The shapes and their texels.
    /// @return The payload bytes, or an empty buffer when the blob is inconsistent.
    [[nodiscard]] vector<u8> EncodeGeneratedTextureBlob(const GeneratedTextureBlob& blob);

    /// @brief Decodes a cache payload back into shapes and texels.
    ///
    /// Every length is checked against what remains, so a truncated or foreign payload decodes to
    /// nullopt rather than reading past its end.
    /// @param payload  The bytes a cache read returned.
    /// @return The decoded blob, or nullopt when the payload is not one.
    [[nodiscard]] optional<GeneratedTextureBlob>
    DecodeGeneratedTextureBlob(std::span<const u8> payload);
}
