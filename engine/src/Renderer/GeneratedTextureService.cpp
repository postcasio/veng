#include <Veng/Renderer/GeneratedTextureService.h>

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Persistence/DerivedDataCache.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Task/TaskSystem.h>

#include "GeneratedTextureBlob.h"
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

        // The stored shape of a live target, which is what a cached payload must match exactly.
        GeneratedTextureBlobShape ShapeOf(const Image& image)
        {
            return {
                .TexelFormat = image.GetFormat(),
                .Type = image.GetType(),
                .Extent = image.GetExtent(),
                .Layers = image.GetLayers(),
                .MipLevels = image.GetMipLevels(),
            };
        }

        // Whether every target can take part in a cache round trip. A created target has the
        // transfer usages folded in; an adopted one is the caller's to size and to declare.
        bool TargetsAreCacheable(const vector<Unique<GeneratedTexture>>& targets,
                                 const string& cacheKey)
        {
            for (const Unique<GeneratedTexture>& target : targets)
            {
                const Image& image = *target->GetImage();
                if (!HasFlag(image.GetUsage(), ImageUsage::TransferSrc) ||
                    !HasFlag(image.GetUsage(), ImageUsage::TransferDst))
                {
                    Log::Warn("generated-texture job '{}' is not cached: target '{}' carries "
                              "neither transfer usage",
                              cacheKey, image.GetName());
                    return false;
                }
                if (GeneratedTextureShapeBytes(ShapeOf(image)) == 0)
                {
                    Log::Warn("generated-texture job '{}' is not cached: target '{}' has a format "
                              "with no known byte size",
                              cacheKey, image.GetName());
                    return false;
                }
            }
            return true;
        }

        // The per-mip copy regions restoring one target from a staging buffer, all layers of a mip
        // at once — the layout the levels were stored in and the one CopyBufferToImage expects.
        vector<BufferImageCopyRegion> RestoreRegions(const GeneratedTextureBlobShape& shape,
                                                     const u64 baseOffset)
        {
            vector<BufferImageCopyRegion> regions;
            regions.reserve(shape.MipLevels);
            for (u32 mip = 0; mip < shape.MipLevels; mip++)
            {
                regions.push_back({
                    .BufferOffset = baseOffset + GeneratedTextureMipOffset(shape, mip),
                    .MipLevel = mip,
                    .Extent = {std::max(1u, shape.Extent.x >> mip),
                               std::max(1u, shape.Extent.y >> mip),
                               std::max(1u, shape.Extent.z >> mip)},
                });
            }
            return regions;
        }

        // The shared state a job's readbacks accumulate into. It holds no reference to the job, so
        // releasing a completed job while its levels are still arriving neither strands the store
        // nor dangles: the result was computed, and it is stored.
        struct StoreCollector
        {
            string CacheKey;
            GeneratedTextureBlob Blob;
            u32 Outstanding = 0;
            bool Failed = false;
        };
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

        const bool cached = !request.CacheKey.empty() && m_Cache != nullptr && m_Tasks != nullptr;

        auto job = CreateUnique<Job>();
        job->Key = request.Key;
        job->Serial = m_NextSerial++;
        job->OnTick = std::move(request.OnTick);
        job->OnComplete = std::move(request.OnComplete);
        job->CacheKey = cached ? std::move(request.CacheKey) : string{};
        job->Targets.reserve(request.Targets.size());
        for (usize i = 0; i < request.Targets.size(); i++)
        {
            GeneratedTextureTargetInfo target = request.Targets[i];
            target.Image.Name = fmt::format("{}[{}]", request.Name, i);
            if (cached)
            {
                // A cached target is read back to be stored and written into to be restored, so it
                // needs both transfer usages whatever its producer access is.
                target.Image.Usage =
                    target.Image.Usage | ImageUsage::TransferSrc | ImageUsage::TransferDst;
            }
            job->Targets.push_back(
                Unique<GeneratedTexture>(new GeneratedTexture(m_Context, target)));
        }

        if (cached && !TargetsAreCacheable(job->Targets, job->CacheKey))
        {
            job->CacheKey.clear();
        }

        Job& added = *m_Jobs.emplace_back(std::move(job));
        if (!added.CacheKey.empty())
        {
            SubmitProbe(added);
        }
        return true;
    }

    void GeneratedTextureService::SetCache(DerivedDataCache* cache, TaskSystem* tasks)
    {
        const bool attached = cache != nullptr && tasks != nullptr;
        m_Cache = attached ? cache : nullptr;
        m_Tasks = attached ? tasks : nullptr;
    }

    void GeneratedTextureService::SubmitProbe(Job& job)
    {
        // Held until the answer lands: the key is live, so a re-request is still idempotent, but no
        // tick is spent on work the cache may already hold.
        m_Queue->SetHeld(job.Key, true);
        job.Probing = true;

        const GeneratedTextureKey key = job.Key;
        const u64 serial = job.Serial;
        DerivedDataCache* cache = m_Cache;
        string cacheKey = job.CacheKey;

        Task<optional<vector<u8>>> probe =
            m_Tasks->Submit([cache, cacheKey = std::move(cacheKey)]
                            { return cache->Read(cacheKey); }, "GeneratedTextureCacheProbe");
        probe.Then(
            [this, key, serial](Result<optional<vector<u8>>> payload)
            {
                ResolveProbe(key, serial,
                             payload.has_value() ? std::move(*payload) : optional<vector<u8>>{});
            });
    }

    void GeneratedTextureService::ResolveProbe(const GeneratedTextureKey key, const u64 serial,
                                               optional<vector<u8>> payload)
    {
        Job* job = FindJob(key);
        if (job == nullptr || job->Serial != serial || !job->Probing)
        {
            return;
        }
        job->Probing = false;

        const auto miss = [&] { m_Queue->SetHeld(key, false); };

        if (!payload.has_value())
        {
            miss();
            return;
        }

        const optional<GeneratedTextureBlob> blob = DecodeGeneratedTextureBlob(*payload);
        if (!blob.has_value() || blob->Shapes.size() != job->Targets.size())
        {
            miss();
            return;
        }
        for (usize i = 0; i < job->Targets.size(); i++)
        {
            if (blob->Shapes[i] != ShapeOf(*job->Targets[i]->GetImage()))
            {
                miss();
                return;
            }
        }

        auto staging = Buffer::Create(m_Context, {
                                                     .Name = "GeneratedTextureRestore",
                                                     .Size = blob->Texels.size(),
                                                     .Usage = BufferUsage::TransferSrc,
                                                     .HostMapped = true,
                                                 });
        staging->UploadSync(blob->Texels);

        PendingRestore restore{.Staging = std::move(staging)};
        restore.TargetOffsets.reserve(blob->Shapes.size());
        u64 offset = 0;
        for (const GeneratedTextureBlobShape& shape : blob->Shapes)
        {
            restore.TargetOffsets.push_back(offset);
            offset += GeneratedTextureShapeBytes(shape);
        }
        job->Restore = std::move(restore);
    }

    void GeneratedTextureService::ApplyRestores(CommandBuffer& cmd,
                                                vector<GeneratedTextureKey>& restored)
    {
        for (const Unique<Job>& job : m_Jobs)
        {
            if (!job->Restore.has_value())
            {
                continue;
            }
            for (usize i = 0; i < job->Targets.size(); i++)
            {
                const GeneratedTexture& target = *job->Targets[i];
                const vector<BufferImageCopyRegion> regions =
                    RestoreRegions(ShapeOf(*target.GetImage()), job->Restore->TargetOffsets[i]);
                cmd.PrepareForAccess(target.GetSampledView(), AccessKind::TransferDst);
                cmd.CopyBufferToImage(job->Restore->Staging, target.GetImage(), regions);
                cmd.PrepareForAccess(target.GetSampledView(), AccessKind::Sample);
            }
            // The staging buffer retires into this frame's bin, so it outlives the copy it was
            // recorded into and is destroyed once that frame's fence has been waited.
            job->Restore.reset();
            // The texels came out of the cache, so there is nothing left to put back into it.
            job->CacheKey.clear();
            m_Queue->MarkResident(job->Key);
            m_CompletedTotal++;
            m_RestoredTotal++;
            restored.push_back(job->Key);
        }
    }

    void GeneratedTextureService::SubmitStore(const Job& job)
    {
        auto collector = CreateRef<StoreCollector>();
        collector->CacheKey = job.CacheKey;
        collector->Blob.Shapes.reserve(job.Targets.size());

        usize total = 0;
        for (const Unique<GeneratedTexture>& target : job.Targets)
        {
            const GeneratedTextureBlobShape shape = ShapeOf(*target->GetImage());
            collector->Blob.Shapes.push_back(shape);
            total += GeneratedTextureShapeBytes(shape);
        }
        collector->Blob.Texels.resize(total);

        AsyncReadback& readback = m_Context.GetAsyncReadback();
        usize base = 0;
        for (const Unique<GeneratedTexture>& target : job.Targets)
        {
            const GeneratedTextureBlobShape shape = ShapeOf(*target->GetImage());
            for (u32 mip = 0; mip < shape.MipLevels; mip++)
            {
                const usize layerBytes = GeneratedTextureLayerBytes(shape, mip);
                for (u32 layer = 0; layer < shape.Layers; layer++)
                {
                    const usize destination = base + GeneratedTextureMipOffset(shape, mip) +
                                              (static_cast<usize>(layer) * layerBytes);
                    collector->Outstanding++;
                    const AsyncReadbackHandle handle = readback.Request({
                        .Name = fmt::format("{} (Cache)", target->GetImage()->GetName()),
                        .Image = target->GetImage(),
                        .MipLevel = mip,
                        .ArrayLayer = layer,
                        .RestoreTo = AccessKind::Sample,
                        .OnComplete =
                            [this, collector, destination, layerBytes](std::span<const u8> bytes)
                        {
                            if (bytes.size() == layerBytes)
                            {
                                std::ranges::copy(bytes, collector->Blob.Texels.begin() +
                                                             static_cast<isize>(destination));
                            }
                            else
                            {
                                collector->Failed = true;
                            }
                            collector->Outstanding--;
                            if (collector->Outstanding > 0 || collector->Failed)
                            {
                                return;
                            }

                            vector<u8> encoded = EncodeGeneratedTextureBlob(collector->Blob);
                            if (encoded.empty() || m_Cache == nullptr || m_Tasks == nullptr)
                            {
                                return;
                            }
                            m_StoredTotal++;
                            DerivedDataCache* cache = m_Cache;
                            m_Tasks->Submit([cache, storeKey = collector->CacheKey,
                                             encoded = std::move(encoded)]
                                            { cache->Store(storeKey, encoded); },
                                            "GeneratedTextureCacheStore");
                        },
                    });
                    if (!handle.IsValid())
                    {
                        collector->Failed = true;
                        collector->Outstanding--;
                    }
                }
            }
            base += GeneratedTextureShapeBytes(shape);
        }
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
                                    .CancelledTotal = m_CancelledTotal,
                                    .RestoredTotal = m_RestoredTotal,
                                    .StoredTotal = m_StoredTotal};
        for (const Unique<Job>& job : m_Jobs)
        {
            if (job->Probing)
            {
                stats.Probing++;
            }
        }
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

        // A restored job's texels land ahead of the tick loop, so its targets are sampleable by the
        // same frame's passes, exactly as a job whose last tick ran this pump.
        ApplyRestores(cmd, completed);

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
            if (job == nullptr)
            {
                continue;
            }
            if (!job->CacheKey.empty() && m_Cache != nullptr && m_Tasks != nullptr)
            {
                SubmitStore(*job);
            }
            if (!job->OnComplete)
            {
                continue;
            }
            const GeneratedTextureResult result{.Key = key, .Targets = job->Targets};
            job->OnComplete(result);
        }
    }
}
