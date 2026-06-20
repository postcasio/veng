#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderModule.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Creation parameters for a compute pipeline.
    struct ComputePipelineInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Layout declaring descriptor sets and push constants.
        Ref<PipelineLayout> PipelineLayout;
        /// @brief The compute shader stage.
        PipelineShaderStageInfo ShaderStage;
    };

    /// @brief A compiled Vulkan compute pipeline.
    class ComputePipeline
    {
    public:
        /// @brief Creates a compute pipeline from the given parameters.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new pipeline.
        static Ref<ComputePipeline> Create(Context& context, const ComputePipelineInfo& info)
        {
            return Ref<ComputePipeline>(new ComputePipeline(context, info));
        }

        /// @brief Defers destruction of the Vulkan pipeline handle until the GPU is done with it.
        ~ComputePipeline();

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        /// @brief Returns the pipeline's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the pipeline layout.
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        /// @brief Backend handle accessor. Returns a mutable ref from a const method by design —
        /// see the Native idiom in Native.h.
        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        ComputePipeline(Context& context, const ComputePipelineInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Backend Vulkan compute pipeline.
        Unique<Native> m_Native;
        /// @brief Descriptor-set and push-constant layout.
        Ref<PipelineLayout> m_PipelineLayout;
    };
}
