#include <Veng/Renderer/RenderGraph.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    namespace
    {
        // The declared-use table: each AccessKind resolves to the layout the
        // image must be in plus the pipeline stage/access scope that uses it.
        // This is what fixes the "layout alone can't tell a compute write from a
        // read" hole — the scope is declared, not guessed from the layout.
        struct AccessScope
        {
            vk::ImageLayout Layout;
            vk::PipelineStageFlags Stage;
            vk::AccessFlags Access;
        };

        AccessScope ScopeFor(const RenderGraph::AccessKind kind)
        {
            using Kind = RenderGraph::AccessKind;
            switch (kind)
            {
            case Kind::ColorAttachment:
                return {
                    vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead,
                };
            case Kind::DepthAttachment:
                return {
                    vk::ImageLayout::eDepthStencilAttachmentOptimal,
                    vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead,
                };
            case Kind::Sample:
                return {
                    vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eShaderRead,
                };
            case Kind::StorageRead:
                return {
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead,
                };
            case Kind::StorageWrite:
                return {
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,
                };
            case Kind::TransferSrc:
                return {
                    vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits::eTransfer,
                    vk::AccessFlagBits::eTransferRead,
                };
            case Kind::TransferDst:
                return {
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits::eTransfer,
                    vk::AccessFlagBits::eTransferWrite,
                };
            }
            VE_ASSERT(false, "RenderGraph: unhandled AccessKind {}", static_cast<u32>(kind));
        }
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Color(const PassAttachment& attachment)
    {
        m_Pass.Accesses.push_back({attachment.View, AccessKind::ColorAttachment,
                                   attachment.Load, attachment.Store, attachment.Clear});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Depth(const PassAttachment& attachment)
    {
        m_Pass.Accesses.push_back({attachment.View, AccessKind::DepthAttachment,
                                   attachment.Load, attachment.Store, attachment.Clear});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Sample(const Ref<ImageView>& view)
    {
        m_Pass.Accesses.push_back({.View = view, .Kind = AccessKind::Sample});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::StorageRead(const Ref<ImageView>& view)
    {
        m_Pass.Accesses.push_back({.View = view, .Kind = AccessKind::StorageRead});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::StorageWrite(const Ref<ImageView>& view)
    {
        m_Pass.Accesses.push_back({.View = view, .Kind = AccessKind::StorageWrite});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::TransferSrc(const Ref<ImageView>& view)
    {
        m_Pass.Accesses.push_back({.View = view, .Kind = AccessKind::TransferSrc});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::TransferDst(const Ref<ImageView>& view)
    {
        m_Pass.Accesses.push_back({.View = view, .Kind = AccessKind::TransferDst});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::LayerCount(const u32 layerCount)
    {
        m_Pass.LayerCount = layerCount;
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::ViewMask(const u32 viewMask)
    {
        m_Pass.ViewMask = viewMask;
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Execute(function<void(CommandBuffer&)> execute)
    {
        m_Pass.Execute = std::move(execute);
        return *this;
    }

    RenderGraph::PassBuilder RenderGraph::AddPass(const string_view name)
    {
        auto& pass = m_Passes.emplace_back(CreateUnique<Pass>());
        pass->Name = string(name);
        pass->Type = PassType::Graphics;
        return PassBuilder(*pass);
    }

    RenderGraph::PassBuilder RenderGraph::AddComputePass(const string_view name)
    {
        auto& pass = m_Passes.emplace_back(CreateUnique<Pass>());
        pass->Name = string(name);
        pass->Type = PassType::Compute;
        return PassBuilder(*pass);
    }

    RenderGraph::PassBuilder RenderGraph::AddTransferPass(const string_view name)
    {
        auto& pass = m_Passes.emplace_back(CreateUnique<Pass>());
        pass->Name = string(name);
        pass->Type = PassType::Transfer;
        return PassBuilder(*pass);
    }

    void RenderGraph::Execute(CommandBuffer& cmd)
    {
        for (const auto& pass : m_Passes)
        {
            // 1. Derive and emit the barriers this pass's declared uses require.
            for (const auto& access : pass->Accesses)
            {
                const auto& view = access.View;
                const auto scope = ScopeFor(access.Kind);

                Backend::TransitionImage(
                    cmd, *view->GetImage(),
                    scope.Layout, scope.Stage, scope.Access,
                    view->GetBaseArrayLayer(), view->GetArrayLayers(),
                    view->GetBaseMipLevel(), view->GetMipLevels());
            }

            // 2. Graphics passes drive dynamic rendering from their attachments;
            // compute/transfer passes just run their lambda.
            if (pass->Type == PassType::Graphics)
            {
                RenderingInfo info;
                bool extentSet = false;

                for (const auto& access : pass->Accesses)
                {
                    if (access.Kind != AccessKind::ColorAttachment &&
                        access.Kind != AccessKind::DepthAttachment)
                    {
                        continue;
                    }

                    if (!extentSet)
                    {
                        // Extent follows the attachment's mip level:
                        // image_extent >> base_mip, floored at 1.
                        const auto extent = access.View->GetImage()->GetExtent();
                        const u32 mip = access.View->GetBaseMipLevel();
                        info.Extent = {
                            std::max(extent.x >> mip, 1u),
                            std::max(extent.y >> mip, 1u),
                        };
                        extentSet = true;
                    }

                    const RenderingAttachmentInfo attachment{
                        .ImageView = access.View,
                        .LoadOp = access.Load,
                        .StoreOp = access.Store,
                        .ClearValue = access.Clear,
                    };

                    if (access.Kind == AccessKind::ColorAttachment)
                    {
                        info.ColorAttachments.push_back(attachment);
                    }
                    else
                    {
                        info.DepthAttachment = attachment;
                    }
                }

                info.LayerCount = pass->LayerCount;
                info.ViewMask = pass->ViewMask;

                cmd.BeginRendering(info);

                if (pass->Execute)
                {
                    pass->Execute(cmd);
                }

                cmd.EndRendering();
            }
            else if (pass->Execute)
            {
                pass->Execute(cmd);
            }
        }
    }
}
