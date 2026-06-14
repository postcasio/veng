#include <Veng/Renderer/RenderGraph.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    // The declared-use table (Backend::ScopeFor) and the hazard rule it feeds
    // (Backend::DecideBarrier) live in Backend/BarrierDecision.h so they are
    // unit-testable without a device.
    using Backend::ScopeFor;

    ImageView& PassContext::Resolved(const ResourceId id) const
    {
        VE_ASSERT(id.IsValid() && id.Index < m_Resolved.size(),
                  "PassContext::Resolved: invalid ResourceId");
        const Ref<ImageView>& view = m_Resolved[id.Index];
        VE_ASSERT(view != nullptr, "PassContext::Resolved: resource is not resolved this frame");
        return *view;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Color(const PassAttachment& attachment)
    {
        m_Pass.Accesses.push_back({attachment.Resource, AccessKind::ColorAttachment,
                                   attachment.Load, attachment.Store, attachment.Clear});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Depth(const PassAttachment& attachment)
    {
        m_Pass.Accesses.push_back({attachment.Resource, AccessKind::DepthAttachment,
                                   attachment.Load, attachment.Store, attachment.Clear});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Sample(const ResourceId resource)
    {
        m_Pass.Accesses.push_back({.Resource = resource, .Kind = AccessKind::Sample});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::StorageRead(const ResourceId resource)
    {
        m_Pass.Accesses.push_back({.Resource = resource, .Kind = AccessKind::StorageRead});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::StorageWrite(const ResourceId resource)
    {
        m_Pass.Accesses.push_back({.Resource = resource, .Kind = AccessKind::StorageWrite});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::TransferSrc(const ResourceId resource)
    {
        m_Pass.Accesses.push_back({.Resource = resource, .Kind = AccessKind::TransferSrc});
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::TransferDst(const ResourceId resource)
    {
        m_Pass.Accesses.push_back({.Resource = resource, .Kind = AccessKind::TransferDst});
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

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Execute(function<void(PassContext&)> execute)
    {
        m_Pass.Execute = std::move(execute);
        return *this;
    }

    ResourceId RenderGraph::CreateTransient(const TransientDesc& desc)
    {
        const u32 index = static_cast<u32>(m_Resources.size());
        m_Resources.push_back(Resource{
            .IsImport = false,
            .Name = desc.Name,
            .Desc = desc,
        });
        return ResourceId{index};
    }

    ResourceId RenderGraph::Import(const string_view name)
    {
        const u32 index = static_cast<u32>(m_Resources.size());
        m_Resources.push_back(Resource{
            .IsImport = true,
            .Name = string(name),
        });
        return ResourceId{index};
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

    void RenderGraph::Execute(CommandBuffer& cmd, const std::span<const ImportBinding> imports)
    {
        // Resolve every resource to a concrete view for this frame: allocate any
        // not-yet-backed transient (own allocation, no aliasing), and bind each
        // import to the view supplied in `imports`. Indexed by ResourceId::Index.
        vector<Ref<ImageView>> resolved(m_Resources.size());

        for (usize i = 0; i < m_Resources.size(); i++)
        {
            Resource& resource = m_Resources[i];

            if (resource.IsImport)
            {
                for (const ImportBinding& binding : imports)
                {
                    if (binding.Id.Index == i)
                    {
                        resolved[i] = binding.View;
                        break;
                    }
                }

                VE_ASSERT(resolved[i] != nullptr,
                          "RenderGraph::Execute: import '{}' has no supplied binding",
                          resource.Name);
                continue;
            }

            // Transient: allocate its backing once, then re-resolve to the same
            // image/view every frame (single-copy, persistent tracked state).
            if (!resource.View)
            {
                resource.Image = Image::Create(m_Context, {
                    .Name = resource.Desc.Name,
                    .Extent = {resource.Desc.Extent.x, resource.Desc.Extent.y, 1},
                    .Format = resource.Desc.Format,
                    .Usage = resource.Desc.Usage,
                });

                resource.View = ImageView::Create(m_Context, {
                    .Name = resource.Desc.Name + " View",
                    .Image = resource.Image,
                });
            }

            resolved[i] = resource.View;
        }

        for (const auto& pass : m_Passes)
        {
            // 1. Derive and emit the barriers this pass's declared uses require.
            for (const auto& access : pass->Accesses)
            {
                const Ref<ImageView>& view = resolved[access.Resource.Index];
                const auto scope = ScopeFor(access.Kind);

                Backend::TransitionImage(
                    cmd, *view->GetImage(),
                    scope.Layout, scope.Stage, scope.Access,
                    view->GetBaseArrayLayer(), view->GetArrayLayers(),
                    view->GetBaseMipLevel(), view->GetMipLevels());
            }

            // 2. Graphics passes drive dynamic rendering from their attachments;
            // compute/transfer passes just run their callback.
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

                    const Ref<ImageView>& view = resolved[access.Resource.Index];

                    if (!extentSet)
                    {
                        // Extent follows the attachment's mip level:
                        // image_extent >> base_mip, floored at 1.
                        const auto extent = view->GetImage()->GetExtent();
                        const u32 mip = view->GetBaseMipLevel();
                        info.Extent = {
                            std::max(extent.x >> mip, 1u),
                            std::max(extent.y >> mip, 1u),
                        };
                        extentSet = true;
                    }

                    const RenderingAttachmentInfo attachment{
                        .ImageView = view,
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
                    PassContext context(cmd, resolved);
                    pass->Execute(context);
                }

                cmd.EndRendering();
            }
            else if (pass->Execute)
            {
                PassContext context(cmd, resolved);
                pass->Execute(context);
            }
        }
    }
}
