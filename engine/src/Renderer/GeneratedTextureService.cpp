#include <Veng/Renderer/GeneratedTextureService.h>

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/ImageView.h>

#include "GeneratedTextureQueue.h"

namespace Veng::Renderer
{
    namespace
    {
        // The usage an access kind implies for the image the ticks write through. Everything else a
        // target needs — TransferSrc for a readback, TransferDst for a seeded upload — is the
        // caller's to declare on the ImageInfo.
        ImageUsage UsageFor(const AccessKind access)
        {
            switch (access)
            {
            case AccessKind::ColorAttachment:
                return ImageUsage::ColorAttachment;
            case AccessKind::DepthAttachment:
                return ImageUsage::DepthAttachment;
            case AccessKind::StorageRead:
            case AccessKind::StorageWrite:
                return ImageUsage::Storage;
            case AccessKind::TransferDst:
                return ImageUsage::TransferDst;
            case AccessKind::TransferSrc:
                return ImageUsage::TransferSrc;
            case AccessKind::Sample:
                return ImageUsage::Sampled;
            case AccessKind::IndirectRead:
            case AccessKind::StorageBufferRead:
            case AccessKind::StorageBufferWrite:
                break;
            }
            VE_ASSERT(false, "a generated-texture target cannot be produced through AccessKind {}",
                      static_cast<u32>(access));
        }

        // The view type covering every layer of an image: a volume is one 3D view, a layered image
        // an array, a single-layer image a plain 2D view.
        ImageViewType AllLayerViewType(const Image& image)
        {
            if (image.GetType() == ImageType::Type3D)
            {
                return ImageViewType::Type3D;
            }
            return image.GetLayers() > 1 ? ImageViewType::Array2D : ImageViewType::Type2D;
        }
    }

    GeneratedTexture::GeneratedTexture(Context& context, const GeneratedTextureTargetInfo& info)
        : m_Context(context), m_ProducerAccess(info.ProducerAccess)
    {
        if (info.Adopt)
        {
            m_Image = info.Adopt;
        }
        else
        {
            ImageInfo image = info.Image;
            image.Usage = image.Usage | ImageUsage::Sampled | UsageFor(info.ProducerAccess);
            m_Image = Image::Create(context, image);
        }

        m_Sampled = ImageView::Create(
            context, {
                         .Name = m_Image->GetName() + " (Sampled)",
                         .Image = m_Image,
                         .ViewType = info.SampledViewType.value_or(AllLayerViewType(*m_Image)),
                         .BaseMipLevel = 0,
                         .MipLevels = m_Image->GetMipLevels(),
                         .BaseArrayLayer = 0,
                         .ArrayLayers = m_Image->GetLayers(),
                     });

        if (info.Bindless)
        {
            VE_ASSERT(m_Image->GetLayers() == 1 && m_Image->GetType() == ImageType::Type2D,
                      "generated texture '{}' asked for a bindless slot but is not a single-layer "
                      "2D image; set 0's sampled-image array is strictly 2D",
                      m_Image->GetName());
            m_Handle = context.GetBindlessRegistry().Register(m_Sampled);
        }
    }

    GeneratedTexture::~GeneratedTexture()
    {
        m_Context.GetBindlessRegistry().Release(m_Handle);
    }

    const Ref<ImageView>& GeneratedTexture::GetView(const u32 mipLevel, const u32 layer) const
    {
        const auto match = [&](const CachedView& cached)
        { return cached.MipLevel == mipLevel && cached.Layer == layer; };
        if (const auto it = std::ranges::find_if(m_Views, match); it != m_Views.end())
        {
            return it->View;
        }

        VE_ASSERT(mipLevel < m_Image->GetMipLevels(), "generated texture '{}' has no mip {}",
                  m_Image->GetName(), mipLevel);
        VE_ASSERT(layer == AllLayers || layer < m_Image->GetLayers(),
                  "generated texture '{}' has no layer {}", m_Image->GetName(), layer);

        const bool allLayers = layer == AllLayers;
        const ImageViewType viewType =
            allLayers ? AllLayerViewType(*m_Image)
                      : (m_Image->GetType() == ImageType::Type3D ? ImageViewType::Type3D
                                                                 : ImageViewType::Type2D);
        m_Views.push_back({
            .MipLevel = mipLevel,
            .Layer = layer,
            .View = ImageView::Create(
                m_Context,
                {
                    .Name = allLayers ? fmt::format("{} (mip {}, all layers)", m_Image->GetName(),
                                                    mipLevel)
                                      : fmt::format("{} (mip {}, layer {})", m_Image->GetName(),
                                                    mipLevel, layer),
                    .Image = m_Image,
                    .ViewType = viewType,
                    .BaseMipLevel = mipLevel,
                    .MipLevels = 1,
                    .BaseArrayLayer = allLayers ? 0 : layer,
                    .ArrayLayers = allLayers ? m_Image->GetLayers() : 1,
                }),
        });
        return m_Views.back().View;
    }

    GeneratedTextureService::GeneratedTextureService(Context& context)
        : m_Context(context), m_Queue(CreateUnique<GeneratedTextureQueue>())
    {
    }

    GeneratedTextureService::~GeneratedTextureService() = default;

    bool GeneratedTextureService::Request(GeneratedTextureRequest request)
    {
        VE_ASSERT(!m_InTick, "GeneratedTextureService::Request called from inside a tick callback");

        if (request.Targets.empty() || !request.OnTick)
        {
            Log::Error("generated-texture job '{}' names {} targets and {} tick callback",
                       request.Name, request.Targets.size(), request.OnTick ? "a" : "no");
            return false;
        }

        if (!m_Queue->Add(request.Key, request.TickCount, request.Priority))
        {
            return false;
        }

        auto job = CreateUnique<Job>();
        job->Key = request.Key;
        job->OnTick = std::move(request.OnTick);
        job->OnComplete = std::move(request.OnComplete);
        job->Targets.reserve(request.Targets.size());
        for (usize i = 0; i < request.Targets.size(); i++)
        {
            GeneratedTextureTargetInfo target = request.Targets[i];
            target.Image.Name = fmt::format("{}[{}]", request.Name, i);
            job->Targets.push_back(
                Unique<GeneratedTexture>(new GeneratedTexture(m_Context, target)));
        }
        m_Jobs.push_back(std::move(job));
        return true;
    }

    bool GeneratedTextureService::Cancel(const GeneratedTextureKey key)
    {
        return Drop(key, true);
    }

    bool GeneratedTextureService::Release(const GeneratedTextureKey key)
    {
        return Drop(key, false);
    }

    bool GeneratedTextureService::Drop(const GeneratedTextureKey key, const bool countCancelled)
    {
        VE_ASSERT(!m_InTick, "a generated-texture job was cancelled from inside a tick callback");

        const GeneratedTextureJobRecord* record = m_Queue->Find(key);
        if (record == nullptr)
        {
            return false;
        }
        const bool unfinished = record->State != GeneratedTextureState::Resident;

        m_Queue->Remove(key);
        const auto it =
            std::ranges::find_if(m_Jobs, [key](const Unique<Job>& job) { return job->Key == key; });
        if (it != m_Jobs.end())
        {
            m_Jobs.erase(it);
        }

        if (countCancelled && unfinished)
        {
            m_CancelledTotal++;
        }
        return true;
    }

    bool GeneratedTextureService::SetPriority(const GeneratedTextureKey key, const i32 priority)
    {
        return m_Queue->SetPriority(key, priority);
    }

    bool GeneratedTextureService::IsResident(const GeneratedTextureKey key) const
    {
        const GeneratedTextureJobRecord* record = m_Queue->Find(key);
        return record != nullptr && record->State == GeneratedTextureState::Resident;
    }

    bool GeneratedTextureService::IsPending(const GeneratedTextureKey key) const
    {
        const GeneratedTextureJobRecord* record = m_Queue->Find(key);
        return record != nullptr && record->State != GeneratedTextureState::Resident;
    }

    optional<GeneratedTextureResult>
    GeneratedTextureService::Find(const GeneratedTextureKey key) const
    {
        if (!IsResident(key))
        {
            return std::nullopt;
        }
        const Job* job = FindJob(key);
        if (job == nullptr)
        {
            return std::nullopt;
        }
        return GeneratedTextureResult{.Key = key, .Targets = job->Targets};
    }

    GeneratedTextureStats GeneratedTextureService::GetStats() const
    {
        GeneratedTextureStats stats{.TicksLastPump = m_TicksLastPump,
                                    .CompletedTotal = m_CompletedTotal,
                                    .CancelledTotal = m_CancelledTotal};
        for (const GeneratedTextureJobRecord& record : m_Queue->GetJobs())
        {
            switch (record.State)
            {
            case GeneratedTextureState::Queued:
                stats.Queued++;
                break;
            case GeneratedTextureState::Running:
                stats.Running++;
                break;
            case GeneratedTextureState::Resident:
                stats.Resident++;
                break;
            }
        }
        return stats;
    }

    GeneratedTextureService::Job* GeneratedTextureService::FindJob(const GeneratedTextureKey key)
    {
        const auto it =
            std::ranges::find_if(m_Jobs, [key](const Unique<Job>& job) { return job->Key == key; });
        return it == m_Jobs.end() ? nullptr : it->get();
    }

    const GeneratedTextureService::Job*
    GeneratedTextureService::FindJob(const GeneratedTextureKey key) const
    {
        const auto it =
            std::ranges::find_if(m_Jobs, [key](const Unique<Job>& job) { return job->Key == key; });
        return it == m_Jobs.end() ? nullptr : it->get();
    }

    void GeneratedTextureService::Pump(CommandBuffer& cmd, const u32 budget)
    {
        m_TicksLastPump = 0;
        if (m_Queue->GetPendingCount() == 0)
        {
            return;
        }

        vector<GeneratedTextureKey> completed;

        m_TicksLastPump = m_Queue->Spend(
            budget,
            [&](const GeneratedTextureKey key, const u32 tickIndex, const u32 tickCount)
            {
                Job* job = FindJob(key);
                VE_ASSERT(job != nullptr, "generated-texture job {} has no GPU half", key);

                // Move every target into the access its ticks write through. On the first tick
                // that is the transition out of Undefined; on a later one the same access again,
                // which is a write-after-write hazard and so still emits the barrier ordering this
                // tick's writes behind the previous tick's.
                for (const Unique<GeneratedTexture>& target : job->Targets)
                {
                    cmd.PrepareForAccess(target->GetSampledView(), target->GetProducerAccess());
                }

                const GeneratedTextureTickContext context{
                    .Key = key,
                    .TickIndex = tickIndex,
                    .TickCount = tickCount,
                    .Targets = job->Targets,
                };
                m_InTick = true;
                job->OnTick(cmd, context);
                m_InTick = false;
            },
            [&](const GeneratedTextureKey key)
            {
                Job* job = FindJob(key);
                VE_ASSERT(job != nullptr, "generated-texture job {} has no GPU half", key);
                for (const Unique<GeneratedTexture>& target : job->Targets)
                {
                    cmd.PrepareForAccess(target->GetSampledView(), AccessKind::Sample);
                }
                m_CompletedTotal++;
                completed.push_back(key);
            });

        // Completions run outside the tick loop so one may request or cancel a job — the prefetch
        // ladder's next rung — without mutating the set the loop is selecting from.
        for (const GeneratedTextureKey key : completed)
        {
            const Job* job = FindJob(key);
            if (job == nullptr || !job->OnComplete)
            {
                continue;
            }
            const GeneratedTextureResult result{.Key = key, .Targets = job->Targets};
            job->OnComplete(result);
        }
    }
}
