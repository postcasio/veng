#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Types.h>

#include <string_view>

/// @brief Reflected shader interface: descriptor bindings and push-constant blocks.
///
/// Reflected at cook time (Slang's reflection API for the .slang path; supplied
/// directly for the inline-SPIR-V path) and serialized into the cooked blob
/// (assetpack's CookedShaderHeader). Never re-derived at runtime — the engine
/// bridges the cooked underlying-integer enums to Veng::Renderer enums (ShaderLoader)
/// and builds layouts from the result.
///
/// A vertex-stage shader references its vertex layout by AssetId (VertexLayoutId).
/// nullopt means no vertex input state — the shader has no vertex stage, or performs
/// vertex pulling. A present AssetId references the VertexLayout the shader was cooked
/// against; resolve it via AssetManager to obtain the VertexBufferLayout for pipeline
/// creation.
namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;

    /// @brief One descriptor binding reflected from a shader's interface.
    ///
    /// Set is always >= 1 — set 0 is the bindless registry's reserved set, recognized
    /// and excluded by the cooker. Named so a material can resolve a binding by name
    /// rather than hand-declaring set/binding numbers.
    struct ShaderBinding
    {
        /// @brief Reflected binding name.
        string Name;
        /// @brief Descriptor set index (always >= 1).
        u32 Set = 1;
        /// @brief Binding slot within the set.
        u32 Binding = 0;
        /// @brief Descriptor resource type.
        DescriptorType Type{};
        /// @brief Array count (1 for a non-arrayed binding).
        u32 Count = 1;
        /// @brief Shader stages that access this binding.
        ShaderStage Stages{};
    };

    /// @brief One push-constant block reflected from a shader's interface.
    ///
    /// Size is <= 128 bytes (Vulkan's guaranteed minimum push-constant block size,
    /// validated at cook time).
    struct ShaderPushConstant
    {
        /// @brief Reflected block or field name.
        string Name;
        /// @brief Byte offset within the push-constant range.
        u32 Offset = 0;
        /// @brief Size in bytes.
        u32 Size = 0;
        /// @brief Shader stages that access this range.
        ShaderStage Stages{};
    };

    /// @brief One cooked shader's reflected shape.
    ///
    /// Covers descriptor bindings (sets >= 1), push-constant ranges, and — for a
    /// vertex-stage shader — the AssetId of the VertexLayout the shader was cooked
    /// against. nullopt VertexLayoutId means no vertex input state (no vertex stage,
    /// or vertex pulling).
    struct ShaderInterface
    {
        /// @brief All reflected descriptor bindings.
        vector<ShaderBinding> Bindings;
        /// @brief All reflected push-constant ranges.
        vector<ShaderPushConstant> PushConstants;
        /// @brief AssetId of the cooked vertex layout, or nullopt.
        optional<AssetId> VertexLayoutId;

        /// @brief Looks up a binding by reflected name.
        ///
        /// Used by material binding to validate and resolve params and textures against
        /// the shader's declared interface.
        /// @param name  The reflected binding name to look up.
        /// @return The matching binding, or nullopt if not found.
        [[nodiscard]] optional<ShaderBinding> FindBinding(std::string_view name) const;

        /// @brief Returns push constants as pipeline-layout ranges.
        ///
        /// The returned ranges are ready to append to PipelineLayoutInfo::PushConstantRanges.
        [[nodiscard]] vector<PushConstantRange> BuildPushConstantRanges() const;

        /// @brief Groups the bindings by descriptor set, validating the set numbering.
        ///
        /// The device-free core of BuildDescriptorSetLayouts: returns one binding list
        /// per declared set, indexed so element i holds set (i + 1)'s bindings. Enforces
        /// the engine's set-numbering contract by fatal assert — every binding targets a
        /// set >= 1 (set 0 is reserved for the bindless registry) and the declared sets
        /// form a contiguous run starting at 1 (no gaps). Returns an empty vector when
        /// this interface declares no bindings.
        /// @return Per-set binding lists, element i for set (i + 1).
        [[nodiscard]] vector<vector<DescriptorBinding>> GroupBindingsBySet() const;

        /// @brief Builds one DescriptorSetLayout per declared set, in set order.
        ///
        /// Ready to append (in order) to PipelineLayoutInfo::DescriptorSetLayouts — set 0
        /// is supplied separately by the BindlessRegistry (see PipelineLayout.cpp). Declared
        /// sets must be a contiguous run starting at 1; a gap or a call on an interface with
        /// no bindings is a fatal authoring error (VE_ASSERT), not silent UB. Returns an empty
        /// vector if this interface declares no bindings.
        /// @param context     Context for Vulkan descriptor-set-layout creation.
        /// @param namePrefix  Prefix prepended to each layout's debug name.
        [[nodiscard]] vector<Ref<DescriptorSetLayout>>
        BuildDescriptorSetLayouts(Context& context, std::string_view namePrefix) const;
    };
}
