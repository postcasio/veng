#include <Veng/Renderer/Backend/RenderGraphSchedule.h>

#include <Veng/Assert.h>

#include <fmt/format.h>

namespace Veng::Renderer::Backend
{
    vector<ScheduledPass>
    DeriveRenderGraphSchedule(const std::span<const ScheduleResource> resources,
                              const std::span<const SchedulePass> passes)
    {
        vector<ScheduledPass> schedule;
        schedule.reserve(passes.size());

        // Tracks which slots have been written by a prior pass, to detect a read before any write.
        vector<bool> written(resources.size(), false);

        // A buffer carries no runtime tracked state, so the graph tracks each buffer
        // slot's last declared scope and bakes both halves of a buffer barrier.
        // Default-constructed entries (no stage/access) mean no prior access.
        vector<SubresourceState> bufferScope(resources.size());

        for (const SchedulePass& pass : passes)
        {
            ScheduledPass baked;

            for (const RenderGraph::Access& access : pass.Accesses)
            {
                const u32 slot = access.Resource.Index;
                const ScheduleResource& source = resources[slot];

                const bool isBufferAccess = access.Kind == AccessKind::StorageBufferRead ||
                                            access.Kind == AccessKind::StorageBufferWrite ||
                                            access.Kind == AccessKind::IndirectRead;

                VE_ASSERT(isBufferAccess == source.IsBuffer,
                          "RenderGraph::Compile: pass '{}' declares a {} access on '{}', which is "
                          "a {} resource",
                          pass.Name, isBufferAccess ? "buffer" : "image", source.Name,
                          source.IsBuffer ? "buffer" : "image");

                if (isBufferAccess)
                {
                    const auto scope = ScopeFor(access.Kind);

                    // A transient buffer read before any pass writes it reads undefined contents.
                    const bool isBufferRead = access.Kind == AccessKind::StorageBufferRead ||
                                              access.Kind == AccessKind::IndirectRead;
                    if (!source.IsImport && isBufferRead)
                    {
                        VE_ASSERT(written[slot],
                                  "RenderGraph::Compile: transient buffer '{}' is read by pass "
                                  "'{}' before any pass writes it",
                                  source.Name, pass.Name);
                    }

                    // A buffer used as indirect args must declare BufferUsage::Indirect;
                    // a storage-buffer access must declare BufferUsage::Storage.
                    if (!source.IsImport)
                    {
                        if (access.Kind == AccessKind::IndirectRead)
                        {
                            VE_ASSERT(HasFlag(source.BufferUsage, BufferUsage::Indirect),
                                      "RenderGraph::Compile: transient buffer '{}' is read as "
                                      "indirect args but lacks BufferUsage::Indirect",
                                      source.Name);
                        }
                        else
                        {
                            VE_ASSERT(HasFlag(source.BufferUsage, BufferUsage::Storage),
                                      "RenderGraph::Compile: transient buffer '{}' is a storage "
                                      "buffer access but lacks BufferUsage::Storage",
                                      source.Name);
                        }
                    }

                    // Emit a barrier only on a hazard: the prior access wrote, or this
                    // access writes. A read-after-read needs none; the scope is OR'd so a
                    // later write waits on every prior read.
                    const SubresourceState& prior = bufferScope[slot];
                    const bool hadPriorAccess = static_cast<bool>(prior.Stage);
                    const bool hazard = IsWriteAccess(prior.Access) || IsWriteAccess(scope.Access);

                    if (hadPriorAccess && hazard)
                    {
                        baked.BufferBarriers.push_back({
                            .Slot = slot,
                            .SrcStage = prior.Stage,
                            .SrcAccess = prior.Access,
                            .DstStage = scope.Stage,
                            .DstAccess = scope.Access,
                        });
                        bufferScope[slot] = {.Stage = scope.Stage, .Access = scope.Access};
                    }
                    else
                    {
                        bufferScope[slot] = {.Stage = prior.Stage | scope.Stage,
                                             .Access = prior.Access | scope.Access};
                    }

                    if (IsWriteAccess(scope.Access))
                    {
                        written[slot] = true;
                    }
                    continue;
                }

                // A transient read before any pass writes it produces undefined contents.
                const bool isRead = access.Kind == AccessKind::Sample ||
                                    access.Kind == AccessKind::StorageRead ||
                                    access.Kind == AccessKind::TransferSrc;
                if (!source.IsImport && isRead)
                {
                    VE_ASSERT(written[slot],
                              "RenderGraph::Compile: transient '{}' is read by pass '{}' "
                              "before any pass writes it",
                              source.Name, pass.Name);
                }

                // A transient used as an attachment or sampled must declare the matching ImageUsage.
                if (!source.IsImport)
                {
                    if (access.Kind == AccessKind::ColorAttachment)
                    {
                        VE_ASSERT(HasFlag(source.ImageUsage, ImageUsage::ColorAttachment),
                                  "RenderGraph::Compile: transient '{}' is a color attachment "
                                  "but lacks ImageUsage::ColorAttachment",
                                  source.Name);
                    }
                    else if (access.Kind == AccessKind::DepthAttachment)
                    {
                        VE_ASSERT(HasFlag(source.ImageUsage, ImageUsage::DepthAttachment),
                                  "RenderGraph::Compile: transient '{}' is a depth attachment "
                                  "but lacks ImageUsage::DepthAttachment",
                                  source.Name);
                    }
                    else if (access.Kind == AccessKind::Sample)
                    {
                        VE_ASSERT(HasFlag(source.ImageUsage, ImageUsage::Sampled),
                                  "RenderGraph::Compile: transient '{}' is sampled "
                                  "but lacks ImageUsage::Sampled",
                                  source.Name);
                    }
                }

                const auto scope = ScopeFor(access.Kind);
                baked.Transitions.push_back({.Slot = slot, .Dst = scope});

                if (IsWriteAccess(scope.Access))
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

            schedule.push_back(std::move(baked));
        }

        return schedule;
    }
}
