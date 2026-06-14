#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <string_view>

// The shader interface (planset-5 plan 08): the descriptor bindings,
// push-constant blocks, and vertex inputs reflected from a shader at cook
// time (Slang's reflection API for the .slang path; supplied directly for the
// inline-SPIR-V path) and serialized into the cooked blob
// (assetformat's CookedShaderHeader). Never re-derived at runtime — the
// engine only bridges the cooked, underlying-integer enums to Veng::Renderer
// enums (ShaderLoader) and builds layouts from the result (below).
namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;

    // One descriptor binding reflected from a shader's interface. Set is
    // always >= 1 — set 0 is the bindless registry's reserved set (plan 05)
    // and is recognized and excluded by the cooker, never part of a
    // ShaderInterface. Named so a material (plan 09) can resolve a binding by
    // name rather than hand-declaring set/binding numbers.
    struct ShaderBinding
    {
        string Name;
        u32 Set = 1;
        u32 Binding = 0;
        DescriptorType Type{};
        u32 Count = 1;
        ShaderStage Stages{};
    };

    // One push-constant block (or field) reflected from a shader's interface,
    // <= 128B (planset-2/01's guaranteed minimum push-constant block size,
    // validated at cook time).
    struct ShaderPushConstant
    {
        string Name;
        u32 Offset = 0;
        u32 Size = 0;
        ShaderStage Stages{};
    };

    // One cooked shader's reflected shape: descriptor bindings (sets >= 1),
    // push-constant ranges, and — for a vertex-stage shader — its vertex
    // input layout (empty for a non-vertex stage).
    struct ShaderInterface
    {
        vector<ShaderBinding> Bindings;
        vector<ShaderPushConstant> PushConstants;
        VertexBufferLayout VertexInputs = VertexBufferLayout(vector<VertexBufferElement>{});

        // Resolves a binding by name (the name-based binding plan 09 needs to
        // validate/bind a material's params and textures against the shader).
        [[nodiscard]] optional<ShaderBinding> FindBinding(std::string_view name) const;

        // This interface's push constants as pipeline-layout ranges, ready to
        // append to PipelineLayoutInfo::PushConstantRanges.
        [[nodiscard]] vector<PushConstantRange> BuildPushConstantRanges() const;

        // Builds one DescriptorSetLayout per declared set, in set order,
        // ready to append (in order) to PipelineLayoutInfo::DescriptorSetLayouts
        // — set 0 is supplied separately, by the BindlessRegistry (see
        // PipelineLayout.cpp). Declared sets must be a contiguous run starting
        // at 1; a gap or an empty interface's call is a fatal authoring error
        // (VE_ASSERT), not silent UB. Returns an empty vector if this
        // interface declares no bindings.
        [[nodiscard]] vector<Ref<DescriptorSetLayout>> BuildDescriptorSetLayouts(Context& context, std::string_view namePrefix) const;

        // Asserts `layout` matches VertexInputs element-for-element (format,
        // in location order) — a loud mismatch between a mesh's vertex layout
        // and the shader that draws it (tightens plan 07's canonical-layout
        // check, which only validated a mesh against the engine's one
        // canonical layout).
        void ValidateVertexLayout(const VertexBufferLayout& layout) const;
    };
}
