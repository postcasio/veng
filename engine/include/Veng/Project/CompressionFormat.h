#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/Types.h>

#include <array>
#include <string_view>

namespace Veng
{
    /// @brief The closed set of codec outputs a build configuration's role table may name.
    ///
    /// Deliberately *not* Renderer::Format: that vocabulary carries depth/stencil/
    /// swapchain/index formats that are nonsensical (and dangerous) as a texture
    /// codec, so a role table typed to it could pick D32Sfloat for Color. This is
    /// the small closed set a role legally resolves to; the free
    /// ToRendererFormat() switch lowers it to the engine format at cook time.
    /// Serialized by name (never ordinal) in the JSON authoring files.
    enum class CompressionFormat : u8
    {
        /// @brief 8-bit RGBA, linear-encoded. Uncompressed unorm.
        RGBA8Unorm,
        /// @brief 8-bit RGBA, sRGB-encoded. Uncompressed albedo.
        RGBA8Srgb,
        /// @brief BC7 block codec, linear-encoded.
        BC7Unorm,
        /// @brief BC7 block codec, sRGB-encoded.
        BC7Srgb,
        /// @brief ASTC 4x4 LDR block codec, linear-encoded.
        ASTC4x4Unorm,
        /// @brief ASTC 4x4 LDR block codec, sRGB-encoded.
        ASTC4x4Srgb,
        /// @brief 16-bit-per-channel RGBA float. The uncompressed HDR output.
        RGBA16Sfloat,
    };

    /// @brief The ordered list of every CompressionFormat, for enumeration and name-table lookup.
    inline constexpr std::array<CompressionFormat, 7> CompressionFormats = {
        CompressionFormat::RGBA8Unorm,   CompressionFormat::RGBA8Srgb,
        CompressionFormat::BC7Unorm,     CompressionFormat::BC7Srgb,
        CompressionFormat::ASTC4x4Unorm, CompressionFormat::ASTC4x4Srgb,
        CompressionFormat::RGBA16Sfloat};

    /// @brief The canonical authoring name of a compression format (e.g. "ASTC4x4Srgb").
    ///
    /// The single source of truth for the format's JSON/UI spelling, shared by the
    /// cooker's parser, the editor's writer, and the editor combos. Pure — no JSON
    /// dependency.
    /// @param format  The format to name.
    /// @return The format's stable name; an empty view for an out-of-range value.
    [[nodiscard]] VE_API std::string_view ToString(CompressionFormat format);

    /// @brief Parses a compression-format name back to its enumerator.
    ///
    /// The inverse of ToString over the same canonical table; matching is exact and
    /// case-sensitive.
    /// @param name  The authoring name, e.g. "BC7Unorm".
    /// @return The format, or nullopt when `name` matches no format.
    [[nodiscard]] VE_API optional<CompressionFormat> ParseCompressionFormat(std::string_view name);

    /// @brief Lowers a compression format to the engine's Renderer::Format vocabulary.
    ///
    /// One exhaustive, Vulkan-free switch — the cook-time bridge from the small
    /// closed codec-output set to the full engine format enum. Asserts on an
    /// out-of-range value (a loud one-line fix, like the backend type mappings).
    /// @param format  The codec output to lower.
    /// @return The matching Renderer::Format.
    [[nodiscard]] VE_API Renderer::Format ToRendererFormat(CompressionFormat format);
}

VE_ENUM(::Veng::CompressionFormat, 0xE374AB3EBA8855F6ULL)
VE_ENUMERATOR(RGBA8Unorm)
VE_ENUMERATOR(RGBA8Srgb)
VE_ENUMERATOR(BC7Unorm)
VE_ENUMERATOR(BC7Srgb)
VE_ENUMERATOR(ASTC4x4Unorm)
VE_ENUMERATOR(ASTC4x4Srgb)
VE_ENUMERATOR(RGBA16Sfloat)
VE_ENUM_END();
