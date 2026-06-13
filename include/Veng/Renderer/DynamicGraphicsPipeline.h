#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct DynamicPipelineInfo
    {
        string Name;

        vector<PipelineAttachmentInfo> ColorAttachments = {};
        Format DepthAttachmentFormat = Format::Undefined;
        Format StencilAttachmentFormat = Format::Undefined;

        optional<VertexBufferLayout> VertexBufferLayout;
        Ref<PipelineLayout> PipelineLayout;

        vector<PipelineShaderStageInfo> ShaderStages = {};
        PolygonMode PolygonMode = PolygonMode::Fill;
        CullMode CullMode = CullMode::None;
        bool DepthTestEnable = false;
        bool DepthWriteEnable = false;
        CompareOp DepthCompareOp = CompareOp::LessOrEqual;
    };

    class DynamicGraphicsPipeline
    {
    public:
        static Ref<DynamicGraphicsPipeline> Create(const DynamicPipelineInfo& info)
        {
            return CreateRef<DynamicGraphicsPipeline>(info);
        }

        explicit DynamicGraphicsPipeline(const DynamicPipelineInfo& info);
        ~DynamicGraphicsPipeline();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_PipelineLayout;
    };
}
