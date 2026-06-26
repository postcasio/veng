#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Project/CompressionFormat.h>

namespace Veng
{
    /// @brief The CompressionRole → CompressionFormat resolution table of a build configuration.
    ///
    /// A fixed small record — one CompressionFormat field per CompressionRole
    /// enumerator — so it reflects as plain leaf-enum fields and the editor draws a
    /// format combo per role. No array support is needed for the table itself; the
    /// role set is closed, so the fields are. The defaults are the shipped LDR codecs
    /// (ASTC 4x4), with HDR uncompressed since the block codecs are LDR.
    struct RoleToFormat
    {
        /// @brief Format the Color role resolves to.
        CompressionFormat Color = CompressionFormat::ASTC4x4Srgb;
        /// @brief Format the Normal role resolves to.
        CompressionFormat Normal = CompressionFormat::ASTC4x4Unorm;
        /// @brief Format the Mask role resolves to.
        CompressionFormat Mask = CompressionFormat::ASTC4x4Unorm;
        /// @brief Format the HDR role resolves to.
        CompressionFormat HDR = CompressionFormat::RGBA16Sfloat;
        /// @brief Format the UI role resolves to.
        CompressionFormat UI = CompressionFormat::ASTC4x4Unorm;
    };

    /// @brief A named per-platform ship target: the codec policy plus output naming for one build configuration.
    ///
    /// The cook reads the active configuration and emits one output pack per
    /// configuration; the CMake layer selects a configuration by Name and reads
    /// OutputSuffix as the single source of truth for the per-config pack suffix.
    /// Reflected so the editor inspects each field through the property table.
    struct BuildConfiguration
    {
        /// @brief The configuration's id, e.g. "macos" — the key the CMake layer selects by.
        string Name;
        /// @brief The target triple or label, e.g. "macos-arm64".
        string Target;
        /// @brief The role → format codec policy for this target.
        RoleToFormat Formats;
        /// @brief The zstd compression level for the output archive.
        i32 CompressionLevel = 19;
        /// @brief The per-config pack suffix, e.g. "-macos" giving "sample-macos.vengpack".
        ///
        /// The single source of truth for the suffix: the CMake layer reads it from
        /// the configuration file rather than re-declaring it.
        string OutputSuffix;
    };
}

VE_REFLECT(::Veng::RoleToFormat, 0x1829ADD48F171D52ULL)
VE_FIELD(Color, .DisplayName = "Color")
VE_FIELD(Normal, .DisplayName = "Normal")
VE_FIELD(Mask, .DisplayName = "Mask")
VE_FIELD(HDR, .DisplayName = "HDR")
VE_FIELD(UI, .DisplayName = "UI")
VE_REFLECT_END();

VE_REFLECT(::Veng::BuildConfiguration, 0xE2FB0DDD547CA088ULL)
VE_FIELD(Name, .DisplayName = "Name")
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(Formats, .DisplayName = "Formats")
VE_FIELD(CompressionLevel, .DisplayName = "Compression Level")
VE_FIELD(OutputSuffix, .DisplayName = "Output Suffix")
VE_REFLECT_END();
