#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/Reflect.h>

#include <array>
#include <string_view>

namespace Veng
{
    /// @brief A texture's compression *intent*, resolved to a concrete codec by a build configuration.
    ///
    /// A texture declares a role — what the pixels mean — never a raw codec. A
    /// BuildConfiguration maps each role to a CompressionFormat per ship target, so
    /// the same source art ships BC7 on desktop and ASTC on Apple GPUs with no
    /// per-asset change. The set is closed and durable: a role's resolved format
    /// fills in as codecs arrive, but the role itself is the stable authoring
    /// surface. Serialized by name (never ordinal) in the JSON authoring files.
    enum class CompressionRole : u8
    {
        /// @brief sRGB-aware albedo / base color. Resolves to the sRGB block codec.
        Color,
        /// @brief Tangent-space normal map. Resolves to the unorm (linear) block codec.
        Normal,
        /// @brief Single- or multi-channel mask (occlusion/roughness/metallic). Resolves to the unorm block codec.
        Mask,
        /// @brief High-dynamic-range source. Resolves to uncompressed RGBA16Sfloat — the LDR block codecs cannot carry HDR.
        HDR,
        /// @brief UI / sprite texture. Resolves to the unorm block codec.
        UI,
    };

    /// @brief The ordered list of every CompressionRole, for enumeration and name-table lookup.
    inline constexpr std::array<CompressionRole, 5> CompressionRoles = {
        CompressionRole::Color, CompressionRole::Normal, CompressionRole::Mask,
        CompressionRole::HDR, CompressionRole::UI};

    /// @brief The canonical authoring name of a compression role (e.g. "Color").
    ///
    /// The single source of truth for the role's JSON/UI spelling, shared by the
    /// cooker's parser, the editor's writer, and the editor combos. Pure — no JSON
    /// dependency.
    /// @param role  The role to name.
    /// @return The role's stable name; an empty view for an out-of-range value.
    [[nodiscard]] VE_API std::string_view ToString(CompressionRole role);

    /// @brief Parses a compression-role name back to its enumerator.
    ///
    /// The inverse of ToString over the same canonical table; matching is exact and
    /// case-sensitive.
    /// @param name  The authoring name, e.g. "Normal".
    /// @return The role, or nullopt when `name` matches no role.
    [[nodiscard]] VE_API optional<CompressionRole> ParseCompressionRole(std::string_view name);
}

VE_ENUM(::Veng::CompressionRole, 0x5C9A85A5EDBF200FULL)
VE_ENUMERATOR(Color)
VE_ENUMERATOR(Normal)
VE_ENUMERATOR(Mask)
VE_ENUMERATOR(HDR)
VE_ENUMERATOR(UI)
VE_ENUM_END();
