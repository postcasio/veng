#pragma once

#include <cstdint>

/// @file
/// @brief The Metal buffer slots a shader's resources are bound at, shared by the cook and the
/// Metal backend.
///
/// A descriptor set reaches Metal as one argument buffer, bound at a buffer slot of each stage's
/// argument table; a binding is an element inside it. Resources that are not stored in an argument
/// buffer — a dynamic uniform buffer, the push-constant block, a vertex buffer — are bound by
/// address at slots of their own. The shader side declares the same slots (Veng/binding.slang), and
/// the cook checks every shader's reflected slots against these constants.
///
/// The header is self-contained, so the engine's backend includes it without the cooker's other
/// headers.

namespace Veng::Cook::MetalBindingSlots
{
    /// @brief The number of buffer slots a Metal argument table offers each stage.
    inline constexpr std::uint32_t BufferSlotCount = 31;

    /// @brief The slot of descriptor set 0's argument buffer; set N is at DescriptorSetBase + N.
    inline constexpr std::uint32_t DescriptorSetBase = 0;
    /// @brief The most descriptor sets a pipeline layout binds.
    inline constexpr std::uint32_t MaxDescriptorSets = 8;

    /// @brief The slot of a pipeline layout's first dynamic uniform buffer.
    ///
    /// A dynamic uniform buffer is bound by address (its base plus the dynamic offset) rather than
    /// written into its set's argument buffer, so a rebind never rewrites memory the GPU may be
    /// reading. The Nth dynamic buffer of a layout, counted in set then binding order, is at
    /// DynamicUniformBufferBase + N.
    inline constexpr std::uint32_t DynamicUniformBufferBase = 8;
    /// @brief The most dynamic uniform buffers a pipeline layout carries.
    inline constexpr std::uint32_t MaxDynamicUniformBuffers = 4;

    /// @brief The slot of the push-constant block.
    ///
    /// The block is copied into a per-frame linear ring and bound by address here. Per-stage ranges
    /// are offsets within this one block.
    inline constexpr std::uint32_t PushConstants = 12;

    /// @brief The slot of vertex buffer binding 0; binding N is at VertexBufferBase + N.
    inline constexpr std::uint32_t VertexBufferBase = 16;
    /// @brief The most vertex buffer bindings a pipeline declares.
    inline constexpr std::uint32_t MaxVertexBuffers = 15;

    /// @brief The size of one argument-buffer element: a resource ID or a GPU address.
    ///
    /// A combined image sampler takes two elements (the texture, then the sampler), and an arrayed
    /// binding takes one per array element.
    inline constexpr std::uint32_t ArgumentBufferElementBytes = 8;

    static_assert(DescriptorSetBase + MaxDescriptorSets <= DynamicUniformBufferBase,
                  "descriptor-set slots overlap the dynamic uniform buffer slots");
    static_assert(DynamicUniformBufferBase + MaxDynamicUniformBuffers <= PushConstants,
                  "dynamic uniform buffer slots overlap the push-constant slot");
    static_assert(PushConstants < VertexBufferBase,
                  "the push-constant slot overlaps the vertex buffer slots");
    static_assert(VertexBufferBase + MaxVertexBuffers <= BufferSlotCount,
                  "vertex buffer slots run past the argument table");
}
