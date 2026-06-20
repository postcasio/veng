#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Creation parameters for a graphics pipeline.
    struct GraphicsPipelineInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Color attachment formats and blend states.
        vector<PipelineAttachmentInfo> ColorAttachments = {};
        /// @brief Depth attachment format (Undefined = none).
        Format DepthAttachmentFormat = Format::Undefined;
        /// @brief Stencil attachment format (Undefined = none).
        Format StencilAttachmentFormat = Format::Undefined;
        /// @brief Vertex input layout (nullopt for pipelines with no vertex buffer).
        optional<VertexBufferLayout> VertexBufferLayout;
        /// @brief Descriptor set and push-constant layout.
        Ref<PipelineLayout> PipelineLayout;
        /// @brief Shader stages (vertex, fragment, etc.).
        vector<PipelineShaderStageInfo> ShaderStages = {};
        /// @brief Fill, line, or point rasterization.
        PolygonMode PolygonMode = PolygonMode::Fill;
        /// @brief Face culling mode.
        CullMode CullMode = CullMode::None;
        /// @brief Enable depth testing.
        bool DepthTestEnable = false;
        /// @brief Enable depth writes.
        bool DepthWriteEnable = false;
        /// @brief Depth comparison operator.
        CompareOp DepthCompareOp = CompareOp::LessOrEqual;
    };

    /// @brief A compiled Vulkan graphics pipeline.
    class GraphicsPipeline
    {
    public:
        /// @brief Creates a graphics pipeline from the given parameters.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new pipeline.
        static Ref<GraphicsPipeline> Create(Context& context, const GraphicsPipelineInfo& info)
        {
            return Ref<GraphicsPipeline>(new GraphicsPipeline(context, info));
        }

        /// @brief Defers destruction of the Vulkan pipeline handle until the GPU is done with it.
        ~GraphicsPipeline();

        GraphicsPipeline(const GraphicsPipeline&) = delete;
        GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

        /// @brief Returns the pipeline's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the pipeline layout.
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        /// @brief Returns the color attachment formats declared at creation.
        ///
        /// CommandBuffer compares these against the active rendering attachments' formats at
        /// draw time (see CommandBuffer::Draw/DrawIndexed) to turn a silent dynamic-rendering
        /// validation error into a named engine assert.
        [[nodiscard]] const vector<Format>& GetColorAttachmentFormats() const
        {
            return m_ColorAttachmentFormats;
        }

        /// @brief Returns the depth attachment format declared at creation.
        [[nodiscard]] Format GetDepthAttachmentFormat() const { return m_DepthAttachmentFormat; }

        /// @brief Opaque backend handle; defined in the matching .cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        GraphicsPipeline(Context& context, const GraphicsPipelineInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Backend Vulkan graphics pipeline.
        Unique<Native> m_Native;
        /// @brief Descriptor-set and push-constant layout.
        Ref<PipelineLayout> m_PipelineLayout;
        /// @brief Color attachment formats declared at creation; compared at draw time for validation.
        vector<Format> m_ColorAttachmentFormats;
        /// @brief Depth attachment format declared at creation.
        Format m_DepthAttachmentFormat = Format::Undefined;
    };
}
