#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

/// @brief Format block geometry and per-level byte sizing, shared by upload-region math and
///        the cooked-texture loader's level walk.
///
/// Block-compressed formats (BC7, ASTC 4x4) store one encoded block per @c BlockWidth x
/// @c BlockHeight texel tile; uncompressed formats report a 1x1 block, so a single helper sizes
/// every format.
/// This header is plain integer arithmetic over Renderer::Format — it pulls in no backend type,
/// keeping it inside the public-header hygiene guarantee.
namespace Veng::Renderer
{
    /// @brief Block geometry of a pixel format: tile extent and the encoded bytes per tile.
    ///
    /// An uncompressed format is a 1x1 block whose @c Bytes is its bytes-per-texel, so the same
    /// ceil-divide sizing math covers compressed and uncompressed formats alike.
    struct FormatBlockInfo
    {
        /// @brief Block width in texels (1 for an uncompressed format, 4 for a block codec).
        u32 BlockWidth = 1;
        /// @brief Block height in texels (1 for an uncompressed format, 4 for a block codec).
        u32 BlockHeight = 1;
        /// @brief Encoded bytes per block (bytes-per-texel for an uncompressed format).
        ///        16 for BC7/BC5/ASTC 4x4, 8 for BC4.
        u32 Bytes = 0;
    };

    /// @brief Returns the block geometry of @p format.
    ///
    /// Covers the formats the texture path produces and uploads. A format this helper does not
    /// know reports a 1x1, zero-byte block, which a caller sizing texture data treats as an
    /// unsupported format rather than a silent mis-size.
    /// @param format The pixel format to describe.
    /// @return The format's block width, height, and bytes-per-block.
    inline constexpr FormatBlockInfo GetFormatBlockInfo(Format format)
    {
        switch (format)
        {
        case Format::BC7Unorm:
        case Format::BC7Srgb:
        case Format::BC5Unorm:
        case Format::ASTC4x4Unorm:
        case Format::ASTC4x4Srgb:
            return {.BlockWidth = 4, .BlockHeight = 4, .Bytes = 16};
        case Format::BC4Unorm:
            return {.BlockWidth = 4, .BlockHeight = 4, .Bytes = 8};
        case Format::R8Unorm:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 1};
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Srgb:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 4};
        default:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 0};
        }
    }

    /// @brief Returns the byte size of one mip level of @p format at @p width x @p height.
    ///
    /// ceil(width / blockWidth) * ceil(height / blockHeight) * bytesPerBlock — the partial edge
    /// blocks of a non-multiple-of-block-size level are counted whole, since a block-compressed
    /// format encodes a full padded block at the edge. For an uncompressed format (a 1x1 block)
    /// this reduces to width * height * bytesPerTexel.
    /// @param format The pixel format.
    /// @param width  The mip level's width in texels.
    /// @param height The mip level's height in texels.
    /// @return The level's tightly-packed byte size.
    inline constexpr usize BytesForLevel(Format format, u32 width, u32 height)
    {
        const FormatBlockInfo block = GetFormatBlockInfo(format);
        const u32 blocksWide = (width + block.BlockWidth - 1) / block.BlockWidth;
        const u32 blocksHigh = (height + block.BlockHeight - 1) / block.BlockHeight;
        return static_cast<usize>(blocksWide) * blocksHigh * block.Bytes;
    }
}
