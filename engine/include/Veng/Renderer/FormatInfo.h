#pragma once

#include <algorithm>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

/// @brief Format block geometry, per-level byte sizing, and the enumerator spellings — shared by
///        upload-region math, the cooked-texture loader's level walk, and diagnostic reporting.
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
        case Format::R16Sfloat:
        case Format::RG8Unorm:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 2};
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Srgb:
        case Format::B10G11R11Ufloat:
        case Format::A2B10G10R10Unorm:
        case Format::A2R10G10B10Unorm:
        case Format::RG16Sfloat:
        case Format::R32Sfloat:
        case Format::R32Uint:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 4};
        case Format::RGBA16Sfloat:
        case Format::RGBA16Uint:
        case Format::RGBA16Unorm:
        case Format::RG32Sfloat:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 8};
        case Format::RGB32Sfloat:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 12};
        case Format::RGBA32Sfloat:
            return {.BlockWidth = 1, .BlockHeight = 1, .Bytes = 16};
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

    /// @brief The upload geometry of a mip-capped texture: where its retained chain begins in the
    ///        tightly-packed blob, its base level's extent, and how many levels it spans.
    struct MipSkipLayout
    {
        /// @brief Byte offset of the first retained level within the tightly-packed mip blob.
        u64 BaseOffset = 0;
        /// @brief Extent of the first retained level in texels.
        uvec2 BaseExtent = {0, 0};
        /// @brief Number of mip levels retained (always >= 1).
        u32 LevelCount = 1;
    };

    /// @brief The effective mip-skip level for a texture, gating the requested level by cappability.
    ///
    /// A texture that is not mip-cappable, or that has a single level, ignores the request and
    /// keeps its full chain (skip 0). Otherwise the request is clamped to leave at least one level.
    /// @param mipCappable    Whether the texture opted into the mip cap (the cooked MipCappable flag).
    /// @param mipCount       The cooked mip-level count (>= 1).
    /// @param requestedSkip  The global texture-quality mip-skip level.
    /// @return The number of top mip levels to drop at upload.
    inline constexpr u32 EffectiveMipSkip(bool mipCappable, u32 mipCount, u32 requestedSkip)
    {
        if (!mipCappable || mipCount <= 1)
        {
            return 0;
        }
        return std::min(requestedSkip, mipCount - 1);
    }

    /// @brief Computes the upload geometry when the top @p skip mip levels of a chain are dropped.
    ///
    /// Level 0 is the full-resolution base; dropping @p skip levels uploads a smaller image
    /// beginning at cooked level min(skip, MipCount - 1), so at least one level always remains. The
    /// base offset is the sum of BytesForLevel over the dropped levels — the offset of the retained
    /// base level in the tightly-packed, offset-table-free blob — the base extent is that level's
    /// dimensions, and the level count is the retained tail. A @p skip of 0 returns { 0, {Width,
    /// Height}, MipCount }: the full chain, byte-identical to an uncapped upload.
    /// @param format   The pixel format (drives the block-aware per-level byte size).
    /// @param width    The full-resolution base width in texels.
    /// @param height   The full-resolution base height in texels.
    /// @param mipCount The cooked mip-level count (>= 1).
    /// @param skip     The requested number of top levels to drop.
    /// @return The retained chain's base offset, base extent, and level count.
    inline constexpr MipSkipLayout ComputeMipSkip(Format format, u32 width, u32 height,
                                                  u32 mipCount, u32 skip)
    {
        const u32 clamped = mipCount > 0 ? std::min(skip, mipCount - 1) : 0;
        u64 baseOffset = 0;
        for (u32 level = 0; level < clamped; level++)
        {
            baseOffset +=
                BytesForLevel(format, std::max(1u, width >> level), std::max(1u, height >> level));
        }
        return MipSkipLayout{
            .BaseOffset = baseOffset,
            .BaseExtent = {std::max(1u, width >> clamped), std::max(1u, height >> clamped)},
            .LevelCount = mipCount - clamped,
        };
    }

    /// @brief Returns the enumerator spelling of @p format, as declared in Renderer/Types.h.
    ///
    /// Format is not a reflected enum — it is a cooked-blob-stable vocabulary enum the backend
    /// maps exhaustively — so a diagnostic that reports a format has no reflection table to name it
    /// through. This is that name, and it is the C++ spelling rather than a prose description so a
    /// reported value can be grepped straight back to its declaration. An unmapped value answers
    /// "Unknown", which is what a diagnostic wants where a switch on a vocabulary enum wants an
    /// assert.
    /// @param format The pixel format to name.
    /// @return The enumerator's name.
    inline constexpr string_view FormatName(Format format)
    {
        switch (format)
        {
        case Format::Undefined:
            return "Undefined";
        case Format::R8Unorm:
            return "R8Unorm";
        case Format::RGBA8Unorm:
            return "RGBA8Unorm";
        case Format::RGBA8Srgb:
            return "RGBA8Srgb";
        case Format::BGRA8Srgb:
            return "BGRA8Srgb";
        case Format::R16Sfloat:
            return "R16Sfloat";
        case Format::RGBA16Sfloat:
            return "RGBA16Sfloat";
        case Format::R32Sfloat:
            return "R32Sfloat";
        case Format::RG32Sfloat:
            return "RG32Sfloat";
        case Format::RGB32Sfloat:
            return "RGB32Sfloat";
        case Format::RGBA32Sfloat:
            return "RGBA32Sfloat";
        case Format::D16Unorm:
            return "D16Unorm";
        case Format::D32Sfloat:
            return "D32Sfloat";
        case Format::S8Uint:
            return "S8Uint";
        case Format::D16UnormS8Uint:
            return "D16UnormS8Uint";
        case Format::D24UnormS8Uint:
            return "D24UnormS8Uint";
        case Format::D32SfloatS8Uint:
            return "D32SfloatS8Uint";
        case Format::X8D24UnormPack32:
            return "X8D24UnormPack32";
        case Format::RG16Sfloat:
            return "RG16Sfloat";
        case Format::A2B10G10R10Unorm:
            return "A2B10G10R10Unorm";
        case Format::RGBA16Uint:
            return "RGBA16Uint";
        case Format::BC7Unorm:
            return "BC7Unorm";
        case Format::BC7Srgb:
            return "BC7Srgb";
        case Format::ASTC4x4Unorm:
            return "ASTC4x4Unorm";
        case Format::ASTC4x4Srgb:
            return "ASTC4x4Srgb";
        case Format::R32Uint:
            return "R32Uint";
        case Format::RG8Unorm:
            return "RG8Unorm";
        case Format::BC5Unorm:
            return "BC5Unorm";
        case Format::BC4Unorm:
            return "BC4Unorm";
        case Format::B10G11R11Ufloat:
            return "B10G11R11Ufloat";
        case Format::RGBA16Unorm:
            return "RGBA16Unorm";
        case Format::A2R10G10B10Unorm:
            return "A2R10G10B10Unorm";
        }
        return "Unknown";
    }
}
