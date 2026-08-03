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
    /// Everything a restore needs to know that the payload alone does not say. A stored shape the
    /// target waiting for it does not answer to makes the whole entry unusable: the texels would be
    /// uploaded into an image they do not describe.
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

    /// @brief A payload's header, read without copying the texels behind it.
    ///
    /// What a restore needs in order to decide whether the entry is usable and where its texels
    /// begin, so a hit costs a header parse rather than a copy of the whole payload.
    struct GeneratedTextureBlobLayout
    {
        /// @brief One shape per target, in the order the job declared them.
        vector<GeneratedTextureBlobShape> Shapes;
        /// @brief Byte offset of the first target's texels within the payload.
        usize TexelOffset = 0;
        /// @brief Bytes of texels the shapes account for, which is the rest of the payload.
        usize TexelBytes = 0;
    };

    /// @brief The most bytes a payload's header can occupy, whatever it describes.
    ///
    /// The header is self-delimiting once its shape count is read and the count is bounded, so a
    /// caller reading a payload out of a random-access source reads this many bytes and parses
    /// whatever arrives (ReadGeneratedTextureBlobPrefix), rather than reading the payload whole to
    /// find out where its texels start.
    inline constexpr usize MaxGeneratedTextureBlobHeaderBytes =
        8 + (2 * sizeof(u32)) + (64 * 7 * sizeof(u32));

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

    /// @brief Begins a payload: the header for @p shapes, in a buffer sized for the texels too.
    ///
    /// The returned buffer holds the header and has capacity reserved for @p texelBytes more, so a
    /// caller appends the texels straight onto it and the payload is assembled with one copy of
    /// them rather than two.
    /// @param shapes      One shape per target, in the order the job declared them.
    /// @param texelBytes  Bytes of texels that will follow; must equal what the shapes describe.
    /// @return The header bytes, or an empty buffer when the shapes and the byte count disagree.
    [[nodiscard]] vector<u8>
    BeginGeneratedTextureBlob(const vector<GeneratedTextureBlobShape>& shapes, usize texelBytes);

    /// @brief Encodes shapes and texels into one cache payload.
    ///
    /// Returns an empty buffer when the texels do not add up to what the shapes describe, so a
    /// caller that assembled a short readback set stores nothing rather than an entry that decodes
    /// into the wrong texels.
    /// @param blob  The shapes and their texels.
    /// @return The payload bytes, or an empty buffer when the blob is inconsistent.
    [[nodiscard]] vector<u8> EncodeGeneratedTextureBlob(const GeneratedTextureBlob& blob);

    /// @brief Reads a cache payload's header, leaving its texels where they are.
    ///
    /// Every length is checked against what remains, so a truncated or foreign payload reads as
    /// nullopt rather than past its end.
    /// @param payload  The bytes a cache read returned.
    /// @return The shapes and where their texels begin, or nullopt when the payload is not one.
    [[nodiscard]] optional<GeneratedTextureBlobLayout>
    ReadGeneratedTextureBlobHeader(std::span<const u8> payload);

    /// @brief Reads a payload's header out of a prefix of it, the texels not needing to be present.
    ///
    /// Everything ReadGeneratedTextureBlobHeader checks except the one thing a prefix cannot answer
    /// — that the texels following the header are exactly as many as the shapes describe. The
    /// returned TexelBytes is therefore what the shapes *require* rather than what is in hand, which
    /// is what a caller reading the texels in ranges needs to know before it asks for them.
    /// MaxGeneratedTextureBlobHeaderBytes is a prefix that always suffices.
    /// @param prefix  The payload's leading bytes.
    /// @return The shapes and where their texels begin, or nullopt when the prefix is not a header.
    [[nodiscard]] optional<GeneratedTextureBlobLayout>
    ReadGeneratedTextureBlobPrefix(std::span<const u8> prefix);

    /// @brief Decodes a cache payload back into shapes and texels.
    ///
    /// The copying sibling of ReadGeneratedTextureBlobHeader, for a caller that wants the texels
    /// as a vector rather than in place.
    /// @param payload  The bytes a cache read returned.
    /// @return The decoded blob, or nullopt when the payload is not one.
    [[nodiscard]] optional<GeneratedTextureBlob>
    DecodeGeneratedTextureBlob(std::span<const u8> payload);
}
