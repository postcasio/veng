#include <Veng/Renderer/RenderGraph.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/Backend/TransientAllocation.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    // Backend::ScopeFor and Backend::DecideBarrier live in Backend/BarrierDecision.h
    // so they are unit-testable without a device.
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

    RenderGraph::PassBuilder&
    RenderGraph::PassBuilder::Execute(function<void(PassContext&)> execute)
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
        return ResourceId{.Index = index};
    }

    ResourceId RenderGraph::Import(const string_view name)
    {
        const u32 index = static_cast<u32>(m_Resources.size());
        m_Resources.push_back(Resource{
            .IsImport = true,
            .Name = string(name),
        });
        return ResourceId{.Index = index};
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

    // Backend::SubresourceState keeps this out of the public header.
    struct CompiledGraph::Native
    {
        // A resolved resource-table slot: transients carry their image/view
        // (allocated at compile); imports carry only a name and are bound per frame.
        struct Resource
        {
            bool IsImport = false;
            string Name;
            Ref<Image> Image;    // transient only
            Ref<ImageView> View; // transient only
        };

        // A baked transition: destination scope derived by ScopeFor at compile.
        // The source and subresource range come from the resolved view's tracked state
        // at replay — only the destination bakes.
        struct Transition
        {
            u32 Slot;
            Backend::SubresourceState Dst;
        };

        // A baked graphics attachment: slot + load/store/clear from the declaration.
        struct Attachment
        {
            u32 Slot;
            bool IsDepth;
            LoadOp Load;
            StoreOp Store;
            ClearValue Clear;
        };

        struct Pass
        {
            RenderGraph::PassType Type = RenderGraph::PassType::Graphics;
            u32 LayerCount = 1;
            u32 ViewMask = 0;
            vector<Transition> Transitions;
            vector<Attachment> Attachments; // graphics passes only
            function<void(PassContext&)> Execute;
        };

        // The compiled graph and its transients must not outlive the context.
        Context* Context = nullptr;
        vector<Resource> Resources;
        vector<Pass> Passes;
    };

    Unique<CompiledGraph> RenderGraph::Compile()
    {
        auto native = CreateUnique<CompiledGraph::Native>();
        native->Context = &m_Context;
        native->Resources.resize(m_Resources.size());

        for (usize i = 0; i < m_Resources.size(); i++)
        {
            CompiledGraph::Native::Resource& resource = native->Resources[i];
            resource.IsImport = m_Resources[i].IsImport;
            resource.Name = m_Resources[i].Name;
        }

        // Allocate transient backing with aliasing: compute each transient's live
        // range and size class, collapse non-overlapping same-key transients onto a
        // shared slot via AssignTransientSlots, then create one image per distinct slot.
        //
        // Two transients sharing a slot reuse storage only across non-overlapping
        // lifetimes; the per-frame barrier schedule serializes the reuse: the later
        // transient's first write transitions from Undefined, and that transition waits
        // on the prior content's last read through the same DecideBarrier hazard rule.
        // No aliasing-specific barrier is required.
        {
            // Resource-table slots that are transients, in table order,
            // with their live ranges and size classes.
            vector<u32> transientSlots;
            vector<Backend::TransientLifetime> lifetimes;
            vector<Backend::AllocationKey> keys;
            vector<bool> firstWriteSeen; // parallel to transientSlots

            // Resource slot -> index into the transient arrays, or ~0u for imports.
            vector<u32> transientIndex(m_Resources.size(), ~0u);

            for (usize i = 0; i < m_Resources.size(); i++)
            {
                if (m_Resources[i].IsImport)
                {
                    continue;
                }

                transientIndex[i] = static_cast<u32>(transientSlots.size());
                transientSlots.push_back(static_cast<u32>(i));
                lifetimes.push_back({.FirstUse = 0, .LastUse = 0});
                firstWriteSeen.push_back(false);
                const TransientDesc& desc = m_Resources[i].Desc;
                keys.push_back({.Format = desc.Format, .Extent = desc.Extent, .Usage = desc.Usage});
            }

            // Scan passes in order: FirstUse = first write; LastUse = last touch.
            for (u32 passIndex = 0; passIndex < m_Passes.size(); passIndex++)
            {
                for (const auto& access : m_Passes[passIndex]->Accesses)
                {
                    const u32 ti = transientIndex[access.Resource.Index];
                    if (ti == ~0u)
                    {
                        continue;
                    }

                    Backend::TransientLifetime& life = lifetimes[ti];
                    if (Backend::IsWriteAccess(ScopeFor(access.Kind).Access) && !firstWriteSeen[ti])
                    {
                        life.FirstUse = passIndex;
                        firstWriteSeen[ti] = true;
                    }
                    life.LastUse = passIndex;
                }
            }

            const vector<u32> assignment = Backend::AssignTransientSlots(lifetimes, keys);

            u32 slotCount = 0;
            for (const u32 slot : assignment)
            {
                slotCount = std::max(slotCount, slot + 1);
            }

            // One image+view per distinct slot, named for its first assigned transient.
            vector<Ref<Image>> slotImages(slotCount);
            vector<Ref<ImageView>> slotViews(slotCount);

            for (u32 ti = 0; ti < transientSlots.size(); ti++)
            {
                const u32 slot = assignment[ti];
                if (slotImages[slot] == nullptr)
                {
                    const TransientDesc& desc = m_Resources[transientSlots[ti]].Desc;
                    slotImages[slot] =
                        Image::Create(m_Context, {
                                                     .Name = desc.Name,
                                                     .Extent = {desc.Extent.x, desc.Extent.y, 1},
                                                     .Format = desc.Format,
                                                     .Usage = desc.Usage,
                                                 });
                    slotViews[slot] = ImageView::Create(m_Context, {
                                                                       .Name = desc.Name + " View",
                                                                       .Image = slotImages[slot],
                                                                   });
                }

                CompiledGraph::Native::Resource& resource = native->Resources[transientSlots[ti]];
                resource.Image = slotImages[slot];
                resource.View = slotViews[slot];
            }
        }

        // Tracks which slots have been written by a prior pass, to detect a read before any write.
        vector<bool> written(m_Resources.size(), false);

        for (const auto& pass : m_Passes)
        {
            CompiledGraph::Native::Pass baked;
            baked.Type = pass->Type;
            baked.LayerCount = pass->LayerCount;
            baked.ViewMask = pass->ViewMask;
            baked.Execute = pass->Execute;

            for (const auto& access : pass->Accesses)
            {
                const u32 slot = access.Resource.Index;
                const Resource& source = m_Resources[slot];

                // A transient read before any pass writes it produces undefined contents.
                const bool isRead = access.Kind == AccessKind::Sample ||
                                    access.Kind == AccessKind::StorageRead ||
                                    access.Kind == AccessKind::TransferSrc;
                if (!source.IsImport && isRead)
                {
                    VE_ASSERT(written[slot],
                              "RenderGraph::Compile: transient '{}' is read by pass '{}' "
                              "before any pass writes it",
                              source.Name, pass->Name);
                }

                // A transient used as an attachment or sampled must declare the matching ImageUsage.
                if (!source.IsImport)
                {
                    if (access.Kind == AccessKind::ColorAttachment)
                    {
                        VE_ASSERT(HasFlag(source.Desc.Usage, ImageUsage::ColorAttachment),
                                  "RenderGraph::Compile: transient '{}' is a color attachment "
                                  "but lacks ImageUsage::ColorAttachment",
                                  source.Name);
                    }
                    else if (access.Kind == AccessKind::DepthAttachment)
                    {
                        VE_ASSERT(HasFlag(source.Desc.Usage, ImageUsage::DepthAttachment),
                                  "RenderGraph::Compile: transient '{}' is a depth attachment "
                                  "but lacks ImageUsage::DepthAttachment",
                                  source.Name);
                    }
                    else if (access.Kind == AccessKind::Sample)
                    {
                        VE_ASSERT(HasFlag(source.Desc.Usage, ImageUsage::Sampled),
                                  "RenderGraph::Compile: transient '{}' is sampled "
                                  "but lacks ImageUsage::Sampled",
                                  source.Name);
                    }
                }

                const auto scope = ScopeFor(access.Kind);
                baked.Transitions.push_back({.Slot = slot, .Dst = scope});

                if (Backend::IsWriteAccess(scope.Access))
                {
                    written[slot] = true;
                }

                if (access.Kind == AccessKind::ColorAttachment ||
                    access.Kind == AccessKind::DepthAttachment)
                {
                    baked.Attachments.push_back({
                        .Slot = slot,
                        .IsDepth = access.Kind == AccessKind::DepthAttachment,
                        .Load = access.Load,
                        .Store = access.Store,
                        .Clear = access.Clear,
                    });
                }
            }

            native->Passes.push_back(std::move(baked));
        }

        Log::Info("RenderGraph::Compile: compiled {} pass(es), {} resource(s)",
                  native->Passes.size(), native->Resources.size());

        return Unique<CompiledGraph>(new CompiledGraph(std::move(native)));
    }

    CompiledGraph::CompiledGraph(Unique<Native> native) : m_Native(std::move(native)) {}

    CompiledGraph::~CompiledGraph() = default;

    Ref<Image> CompiledGraph::ResolvedImage(const ResourceId id) const
    {
        if (!id.IsValid() || id.Index >= m_Native->Resources.size())
        {
            return nullptr;
        }
        return m_Native->Resources[id.Index].Image;
    }

    void CompiledGraph::Execute(CommandBuffer& cmd,
                                const std::span<const RenderGraph::ImportBinding> imports,
                                void* userData)
    {
        Native& native = *m_Native;

        // Resolve every slot to a concrete view: transients use their allocated view;
        // imports bind to the caller-supplied view. Indexed by ResourceId::Index.
        vector<Ref<ImageView>> resolved(native.Resources.size());

        for (usize i = 0; i < native.Resources.size(); i++)
        {
            const Native::Resource& resource = native.Resources[i];

            if (!resource.IsImport)
            {
                resolved[i] = resource.View;
                continue;
            }

            for (const RenderGraph::ImportBinding& binding : imports)
            {
                if (binding.Id.Index == i)
                {
                    resolved[i] = binding.View;
                    break;
                }
            }

            VE_ASSERT(resolved[i] != nullptr,
                      "CompiledGraph::Execute: import '{}' has no supplied binding", resource.Name);
        }

        for (const Native::Pass& pass : native.Passes)
        {
            // 1. Replay the baked transitions. The source state and subresource range
            // come from each image's live tracked state, so swapchain and
            // transfer-produced imports stay correct each frame.
            for (const Native::Transition& transition : pass.Transitions)
            {
                const Ref<ImageView>& view = resolved[transition.Slot];

                Backend::TransitionImage(cmd, *view->GetImage(), transition.Dst.Layout,
                                         transition.Dst.Stage, transition.Dst.Access,
                                         view->GetBaseArrayLayer(), view->GetArrayLayers(),
                                         view->GetBaseMipLevel(), view->GetMipLevels());
            }

            // 2. Graphics passes begin dynamic rendering from baked attachments;
            // compute/transfer passes run their callback directly.
            if (pass.Type == RenderGraph::PassType::Graphics)
            {
                RenderingInfo info;
                bool extentSet = false;

                for (const Native::Attachment& attachment : pass.Attachments)
                {
                    const Ref<ImageView>& view = resolved[attachment.Slot];

                    // Extent at the attachment's mip level (image_extent >> base_mip, floored at 1);
                    // all attachments in one pass must agree.
                    const auto extent = view->GetImage()->GetExtent();
                    const u32 mip = view->GetBaseMipLevel();
                    const uvec2 attachmentExtent{
                        std::max(extent.x >> mip, 1u),
                        std::max(extent.y >> mip, 1u),
                    };

                    if (!extentSet)
                    {
                        info.Extent = attachmentExtent;
                        extentSet = true;
                    }
                    else
                    {
                        VE_ASSERT(attachmentExtent == info.Extent,
                                  "CompiledGraph::Execute: attachment '{}' extent {}x{} "
                                  "disagrees with the pass extent {}x{}",
                                  view->GetImage()->GetName(), attachmentExtent.x,
                                  attachmentExtent.y, info.Extent.x, info.Extent.y);
                    }

                    const RenderingAttachmentInfo info2{
                        .ImageView = view,
                        .LoadOp = attachment.Load,
                        .StoreOp = attachment.Store,
                        .ClearValue = attachment.Clear,
                    };

                    if (attachment.IsDepth)
                    {
                        info.DepthAttachment = info2;
                    }
                    else
                    {
                        info.ColorAttachments.push_back(info2);
                    }
                }

                info.LayerCount = pass.LayerCount;
                info.ViewMask = pass.ViewMask;

                cmd.BeginRendering(info);

                if (pass.Execute)
                {
                    PassContext context(cmd, resolved, userData);
                    pass.Execute(context);
                }

                cmd.EndRendering();
            }
            else if (pass.Execute)
            {
                PassContext context(cmd, resolved, userData);
                pass.Execute(context);
            }
        }
    }
}
