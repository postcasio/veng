#pragma once

#include <span>
#include <string_view>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Types.h>

// Reading and writing the cooked shader blob (CookedShaderHeader and what follows it). The cook
// writes it, the runtime loader reads it, and both go through these two functions, so neither can
// accept a layout the other does not produce.

namespace Veng
{
    /// @brief One target's code, as the cook hands it to BuildCookedShaderBlob.
    struct CookedShaderSectionInput
    {
        /// @brief The code kind.
        CookedShaderCode Code = CookedShaderCode::SpirV;
        /// @brief The compiled code.
        std::span<const u8> Bytes;
        /// @brief The entry point's name in this target's code.
        std::string_view EntryPoint;
    };

    /// @brief One target's code within a parsed blob.
    struct CookedShaderSectionView
    {
        /// @brief The code kind.
        CookedShaderCode Code = CookedShaderCode::SpirV;
        /// @brief The compiled code; a view into the parsed blob.
        std::span<const u8> Bytes;
        /// @brief The entry point's name in this target's code.
        string EntryPoint;
    };

    /// @brief A cooked shader blob, parsed and bounds-checked.
    struct CookedShaderView
    {
        /// @brief The interface counts and the vertex-layout reference.
        CookedShaderInterfaceHeader Interface;
        /// @brief The reflected descriptor bindings.
        vector<CookedDescriptorBinding> Bindings;
        /// @brief The reflected push-constant blocks.
        vector<CookedPushConstantBlock> PushConstants;
        /// @brief Each compiled target's code, in the order the cook wrote them.
        vector<CookedShaderSectionView> Sections;
    };

    /// @brief Serializes a cooked shader blob.
    /// @param interfaceHeader The interface counts and vertex-layout reference; the counts are
    ///                        taken from the spans, so the caller need not fill them.
    /// @param bindings        The reflected descriptor bindings.
    /// @param pushConstants   The reflected push-constant blocks.
    /// @param sections        Each compiled target's code; at least one.
    /// @return The blob's bytes.
    [[nodiscard]] vector<u8>
    BuildCookedShaderBlob(CookedShaderInterfaceHeader interfaceHeader,
                          std::span<const CookedDescriptorBinding> bindings,
                          std::span<const CookedPushConstantBlock> pushConstants,
                          std::span<const CookedShaderSectionInput> sections);

    /// @brief Parses a cooked shader blob, checking every size and offset against the blob.
    ///
    /// The returned sections view into @p blob, which must outlive them.
    /// @param blob The cooked bytes.
    /// @return The parsed blob, or a description of what is malformed.
    [[nodiscard]] Result<CookedShaderView> ParseCookedShader(std::span<const u8> blob);

    /// @brief Finds the section holding @p code, or reports that the blob carries none.
    /// @param shader    The parsed blob.
    /// @param code      The code kind the caller's backend consumes.
    /// @param assetName The shader asset's name, which a missing section's error names.
    /// @return The section, or an error naming the asset, the missing kind and the kinds present.
    [[nodiscard]] Result<const CookedShaderSectionView*>
    FindCookedShaderSection(const CookedShaderView& shader, CookedShaderCode code,
                            std::string_view assetName);

    /// @brief The display name of a code kind ("SPIR-V", "Metal library"), for diagnostics.
    [[nodiscard]] std::string_view CookedShaderCodeName(CookedShaderCode code);
}
