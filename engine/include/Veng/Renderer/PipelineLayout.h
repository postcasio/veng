#pragma once

#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief A shader stage and its compiled module, used when building a pipeline.
    struct PipelineShaderStageInfo
    {
        /// @brief The shader stage (vertex, fragment, compute, etc.).
        ShaderStage Stage;
        /// @brief The compiled shader module for this stage.
        Ref<ShaderModule> Module;
    };

    /// @brief Format and blend state for one color attachment in a graphics pipeline.
    struct PipelineAttachmentInfo
    {
        /// @brief Color attachment format.
        Format Format = Format::Undefined;
        /// @brief Blend mode (default opaque).
        BlendState Blend = BlendState::Opaque();
    };

    /// @brief Describes one push-constant range declared on a pipeline layout.
    ///
    /// CommandBuffer::PushConstants<T> reads it back to recover the stages, offset, and
    /// size that would otherwise be restated at the call site.
    struct PushConstantRange
    {
        /// @brief Shader stages that receive this range.
        ShaderStage Stages{};
        /// @brief Byte offset within the push-constant block.
        u32 Offset = 0;
        /// @brief Byte size of this range.
        u32 Size{};

        /// @brief Constructs a range whose size is derived from sizeof(T), so it is never hand-written.
        /// @tparam T      The push-constant value type.
        /// @param stages  Shader stages for the range.
        /// @param offset  Byte offset (default 0).
        template <typename T>
        static PushConstantRange Of(ShaderStage stages, u32 offset = 0)
        {
            return {.Stages = stages, .Offset = offset, .Size = sizeof(T)};
        }
    };

    /// @brief Creation parameters for a pipeline layout.
    struct PipelineLayoutInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Descriptor set layouts in set-number order.
        vector<Ref<DescriptorSetLayout>> DescriptorSetLayouts{};
        /// @brief Push-constant ranges.
        vector<PushConstantRange> PushConstantRanges{};
    };

    /// @brief A compiled Vulkan pipeline layout declaring descriptor sets and push-constant ranges.
    class PipelineLayout
    {
    public:
        /// @brief Creates a pipeline layout from the given parameters.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new layout.
        static Ref<PipelineLayout> Create(Context& context, const PipelineLayoutInfo& info)
        {
            return Ref<PipelineLayout>(new PipelineLayout(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan pipeline layout until the GPU is done with it.
        ~PipelineLayout();

        PipelineLayout(const PipelineLayout&) = delete;
        PipelineLayout& operator=(const PipelineLayout&) = delete;

        /// @brief Returns the layout's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the declared push-constant ranges.
        ///
        /// CommandBuffer::PushConstants<T> searches this list to recover stage flags and size.
        [[nodiscard]] const vector<PushConstantRange>& GetPushConstantRanges() const
        {
            return m_PushConstantRanges;
        }

        /// @brief Opaque backend handle; defined in PipelineLayout.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        PipelineLayout(Context& context, const PipelineLayoutInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;
        vector<Ref<DescriptorSetLayout>> m_DescriptorSetLayouts;
        vector<PushConstantRange> m_PushConstantRanges;
    };
}
