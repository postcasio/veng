#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct AttachmentDescription
    {
        Format Format;
        LoadOp LoadOp = LoadOp::DontCare;
        StoreOp StoreOp = StoreOp::DontCare;
        ImageLayout InitialLayout = ImageLayout::Undefined;
        ImageLayout FinalLayout = ImageLayout::Undefined;
    };

    struct RenderPassInfo
    {
        string Name;
        vector<AttachmentDescription> Attachments;
    };

    class RenderPass
    {
    public:
        static Ref<RenderPass> Create(const RenderPassInfo& info)
        {
            return CreateRef<RenderPass>(info);
        }

        explicit RenderPass(const RenderPassInfo& info);
        ~RenderPass();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const vector<AttachmentDescription>& GetAttachments() const { return m_Attachments; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        vector<AttachmentDescription> m_Attachments;
    };
}
