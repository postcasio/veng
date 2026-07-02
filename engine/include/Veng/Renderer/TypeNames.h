#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

#include <array>
#include <string_view>

namespace Veng::Renderer
{
    /// @brief The canonical authoring names of Filter, indexed by enumerator ordinal.
    ///
    /// The single source of truth for the sampler vocabulary's JSON/UI spelling
    /// (the `*.tex.json` "sampler" keys), shared by the cooker's parser and the
    /// editor's combos + writer. Header-inline so the veng-free cooker core reads
    /// the one table without linking libveng.
    inline constexpr std::array<const char*, 2> FilterNames = {"nearest", "linear"};

    /// @brief The canonical authoring names of MipmapMode, indexed by enumerator ordinal.
    inline constexpr std::array<const char*, 2> MipmapModeNames = {"nearest", "linear"};

    /// @brief The canonical authoring names of AddressMode, indexed by enumerator ordinal.
    inline constexpr std::array<const char*, 4> AddressModeNames = {
        "repeat", "mirrored_repeat", "clamp_to_edge", "clamp_to_border"};

    /// @brief Parses a filter authoring name (e.g. "linear") back to its enumerator.
    /// @param name  The authoring name from FilterNames.
    /// @return The filter, or nullopt when `name` matches no filter.
    [[nodiscard]] constexpr optional<Filter> ParseFilter(std::string_view name)
    {
        for (usize i = 0; i < FilterNames.size(); ++i)
        {
            if (name == FilterNames[i])
            {
                return static_cast<Filter>(i);
            }
        }
        return std::nullopt;
    }

    /// @brief Parses a mipmap-mode authoring name (e.g. "linear") back to its enumerator.
    /// @param name  The authoring name from MipmapModeNames.
    /// @return The mode, or nullopt when `name` matches no mode.
    [[nodiscard]] constexpr optional<MipmapMode> ParseMipmapMode(std::string_view name)
    {
        for (usize i = 0; i < MipmapModeNames.size(); ++i)
        {
            if (name == MipmapModeNames[i])
            {
                return static_cast<MipmapMode>(i);
            }
        }
        return std::nullopt;
    }

    /// @brief Parses an address-mode authoring name (e.g. "clamp_to_edge") back to its enumerator.
    /// @param name  The authoring name from AddressModeNames.
    /// @return The mode, or nullopt when `name` matches no mode.
    [[nodiscard]] constexpr optional<AddressMode> ParseAddressMode(std::string_view name)
    {
        for (usize i = 0; i < AddressModeNames.size(); ++i)
        {
            if (name == AddressModeNames[i])
            {
                return static_cast<AddressMode>(i);
            }
        }
        return std::nullopt;
    }
}
