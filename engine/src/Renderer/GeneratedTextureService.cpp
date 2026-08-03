#include <Veng/Renderer/GeneratedTextureService.h>

#include <algorithm>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Persistence/DerivedDataCache.h>
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

        // Whether a stored shape reduced by `mipOffset` levels is exactly a target's own shape: the
        // match a tail target asks for, and at a zero offset the plain equality a whole-shape target
        // asks for. Stated as a reduction of the stored shape rather than a widening of the
        // target's, because a chain whose extent has clamped at one texel cannot be widened back
        // unambiguously.
        bool MatchesStoredShape(const GeneratedTextureBlobShape& stored, const Image& image,
                                const u32 mipOffset)
        {
            if (mipOffset >= stored.MipLevels)
            {
                return false;
            }
            GeneratedTextureBlobShape tail = stored;
            tail.MipLevels = stored.MipLevels - mipOffset;
            tail.Extent = {std::max(1u, stored.Extent.x >> mipOffset),
                           std::max(1u, stored.Extent.y >> mipOffset),
                           std::max(1u, stored.Extent.z >> mipOffset)};
            return tail == ShapeOf(image);
        }

        // The per-mip copy regions joining one target and a staging buffer, all layers of a mip at
        // once — the layout the levels are stored in and the one the copies in both directions
        // expect.
        vector<BufferImageCopyRegion> LevelRegions(const GeneratedTextureBlobShape& shape,
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

        // A job's created targets on their way back from the worker that created them. Ref-held
        // because a Task's payload is copied to reach its continuation and a target is move-only.
        struct TargetBatch
        {
            vector<Unique<GeneratedTexture>> Targets;
        };
    }

    GeneratedTexture::GeneratedTexture(Context& context, const GeneratedTextureTargetInfo& info)
        : m_Context(context), m_ProducerAccess(info.ProducerAccess), m_WantsBindless(info.Bindless)
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
    }

    void GeneratedTexture::RegisterBindless()
    {
        if (!m_WantsBindless || m_Handle.IsValid())
        {
            return;
        }
        VE_ASSERT(m_Image->GetLayers() == 1 && m_Image->GetType() == ImageType::Type2D,
                  "generated texture '{}' asked for a bindless slot but is not a single-layer "
                  "2D image; set 0's sampled-image array is strictly 2D",
                  m_Image->GetName());
        m_Handle = m_Context.GetBindlessRegistry().Register(m_Sampled);
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

        vector<GeneratedTextureTargetInfo> targets = std::move(request.Targets);
        job->CacheMipOffsets.reserve(targets.size());
        for (usize i = 0; i < targets.size(); i++)
        {
            targets[i].Image.Name = fmt::format("{}[{}]", request.Name, i);
            job->CacheMipOffsets.push_back(targets[i].CacheMipOffset);
            job->CacheTail = job->CacheTail || targets[i].CacheMipOffset != 0;
            if (cached)
            {
                // A cached target is read back to be stored and written into to be restored, so it
                // needs both transfer usages whatever its producer access is.
                targets[i].Image.Usage =
                    targets[i].Image.Usage | ImageUsage::TransferSrc | ImageUsage::TransferDst;
            }
        }

        Job& added = *m_Jobs.emplace_back(std::move(job));

        // An image the service creates can be hundreds of megabytes, and Request is called from the
        // frame pump — so the creation goes to a worker and the job waits for it. An adopted target
        // is not the service's to allocate, so a job made entirely of them takes no hop.
        const bool allocates =
            std::ranges::any_of(targets, [](const GeneratedTextureTargetInfo& target)
                                { return target.Adopt == nullptr; });
        if (allocates && GetWorkers() != nullptr)
        {
            SubmitAllocation(added, std::move(targets));
            return true;
        }

        added.Targets.reserve(targets.size());
        for (const GeneratedTextureTargetInfo& target : targets)
        {
            added.Targets.push_back(
                Unique<GeneratedTexture>(new GeneratedTexture(m_Context, target)));
            added.Targets.back()->RegisterBindless();
        }
        BeginJob(added);
        return true;
    }

    void GeneratedTextureService::UpdateHold(const Job& job)
    {
        m_Queue->SetHeld(job.Key, job.Allocating || job.Probing || job.Restoring);
    }

    void GeneratedTextureService::SubmitAllocation(Job& job,
                                                   vector<GeneratedTextureTargetInfo> targets)
    {
        job.Allocating = true;
        UpdateHold(job);

        const GeneratedTextureKey key = job.Key;
        const u64 serial = job.Serial;
        Context& context = m_Context;

        // A Task's payload is copied on the way to its continuation, so the move-only targets
        // travel behind a Ref rather than as the payload itself.
        Task<Ref<TargetBatch>> allocation = GetWorkers()->Submit(
            [&context, targets = std::move(targets)]
            {
                auto batch = CreateRef<TargetBatch>();
                batch->Targets.reserve(targets.size());
                for (const GeneratedTextureTargetInfo& target : targets)
                {
                    batch->Targets.push_back(
                        Unique<GeneratedTexture>(new GeneratedTexture(context, target)));
                }
                return batch;
            },
            "GeneratedTextureAllocate");
        allocation.Then(
            [this, key, serial](Result<Ref<TargetBatch>> batch)
            {
                ResolveAllocation(key, serial,
                                  batch.has_value() && *batch != nullptr
                                      ? std::move((*batch)->Targets)
                                      : vector<Unique<GeneratedTexture>>{});
            });
    }

    void GeneratedTextureService::ResolveAllocation(const GeneratedTextureKey key, const u64 serial,
                                                    vector<Unique<GeneratedTexture>> targets)
    {
        Job* job = FindJob(key);
        if (job == nullptr || job->Serial != serial || !job->Allocating)
        {
            // The job was cancelled, released, or re-requested while the worker ran. The targets go
            // out of scope here, on the main thread, through the ordinary retire path.
            return;
        }
        job->Allocating = false;
        job->Targets = std::move(targets);
        for (const Unique<GeneratedTexture>& target : job->Targets)
        {
            target->RegisterBindless();
        }
        BeginJob(*job);
    }

    void GeneratedTextureService::BeginJob(Job& job)
    {
        if (!job.CacheKey.empty() && !TargetsAreCacheable(job.Targets, job.CacheKey))
        {
            job.CacheKey.clear();
        }
        if (!job.CacheKey.empty())
        {
            SubmitProbe(job);
            return;
        }
        UpdateHold(job);
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
        job.Probing = true;
        UpdateHold(job);

        const GeneratedTextureKey key = job.Key;
        const u64 serial = job.Serial;
        DerivedDataCache* cache = m_Cache;
        string cacheKey = job.CacheKey;

        // A tail job wants a fraction of the entry, so it reads the header first and the levels it
        // wants after — two seeks against one file rather than a copy of every byte stored. A
        // whole-shape job's wanted range *is* the payload, so it reads it in one go and gets the
        // entry's digest checked into the bargain.
        if (job.CacheTail)
        {
            Task<optional<vector<u8>>> probe = m_Tasks->Submit(
                [cache, cacheKey = std::move(cacheKey)]
                { return cache->ReadRange(cacheKey, 0, MaxGeneratedTextureBlobHeaderBytes); },
                "GeneratedTextureCacheProbe");
            probe.Then(
                [this, key, serial](Result<optional<vector<u8>>> header)
                {
                    ResolveTailProbe(key, serial,
                                     header.has_value() ? std::move(*header)
                                                        : optional<vector<u8>>{});
                });
            return;
        }

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

        const auto miss = [&] { UpdateHold(*job); };

        if (!payload.has_value())
        {
            miss();
            return;
        }

        // Only the header is read here: the texels stay in the payload until a worker copies them
        // into the staging buffer, so a hit costs the frame thread a parse rather than the payload.
        const optional<GeneratedTextureBlobLayout> layout =
            ReadGeneratedTextureBlobHeader(*payload);
        if (!layout.has_value() || layout->Shapes.size() != job->Targets.size())
        {
            miss();
            return;
        }
        for (usize i = 0; i < job->Targets.size(); i++)
        {
            if (!MatchesStoredShape(layout->Shapes[i], *job->Targets[i]->GetImage(), 0))
            {
                miss();
                return;
            }
        }

        // Sized for the whole payload: the header's few bytes ride along at the front so the
        // vector uploads whole, and the copy regions simply start past them.
        auto staging =
            Buffer::Create(m_Context, {
                                          .Name = "GeneratedTextureRestore",
                                          .Size = layout->TexelOffset + layout->TexelBytes,
                                          .Usage = BufferUsage::TransferSrc,
                                          .HostMapped = true,
                                      });

        PendingRestore restore{.Staging = staging};
        restore.TargetOffsets.reserve(layout->Shapes.size());
        u64 offset = layout->TexelOffset;
        for (const GeneratedTextureBlobShape& shape : layout->Shapes)
        {
            restore.TargetOffsets.push_back(offset);
            offset += GeneratedTextureShapeBytes(shape);
        }
        job->Restore = std::move(restore);
        job->Restoring = true;
        UpdateHold(*job);

        // The host copy is the size of the whole target set, so it runs on a worker: the payload
        // is moved onto it rather than copied, and the buffer is held until the memcpy has run.
        Task<void> upload = staging->Upload(*GetWorkers(), std::move(*payload));
        upload.Then([this, key, serial](Result<std::monostate>) { ResolveRestore(key, serial); });
    }

    void GeneratedTextureService::ResolveTailProbe(const GeneratedTextureKey key, const u64 serial,
                                                   optional<vector<u8>> header)
    {
        Job* job = FindJob(key);
        if (job == nullptr || job->Serial != serial || !job->Probing)
        {
            return;
        }
        job->Probing = false;

        const auto miss = [&] { UpdateHold(*job); };

        if (!header.has_value())
        {
            miss();
            return;
        }

        // The prefix parse rather than the exact one: the texels the shapes describe are on disk and
        // deliberately not in hand, so "the payload is exactly this long" is a check the header alone
        // cannot make and the cache's own length check already made.
        const optional<GeneratedTextureBlobLayout> layout = ReadGeneratedTextureBlobPrefix(*header);
        if (!layout.has_value() || layout->Shapes.size() != job->Targets.size())
        {
            miss();
            return;
        }
        for (usize i = 0; i < job->Targets.size(); i++)
        {
            if (!MatchesStoredShape(layout->Shapes[i], *job->Targets[i]->GetImage(),
                                    job->CacheMipOffsets[i]))
            {
                miss();
                return;
            }
        }

        // What each target wants is the tail of its stored shape's texels: the levels from its own
        // offset onward, which are contiguous because the levels run mip-major. So one range per
        // target, and the staging buffer holds those ranges and nothing else.
        struct Wanted
        {
            u64 Source = 0;
            u64 Bytes = 0;
        };
        vector<Wanted> wanted;
        wanted.reserve(layout->Shapes.size());
        vector<u64> targetOffsets;
        targetOffsets.reserve(layout->Shapes.size());
        u64 source = 0;
        u64 total = 0;
        for (usize i = 0; i < layout->Shapes.size(); i++)
        {
            const GeneratedTextureBlobShape& shape = layout->Shapes[i];
            const usize shapeBytes = GeneratedTextureShapeBytes(shape);
            const usize skipped = GeneratedTextureMipOffset(shape, job->CacheMipOffsets[i]);
            targetOffsets.push_back(total);
            wanted.push_back({.Source = source + skipped, .Bytes = shapeBytes - skipped});
            total += shapeBytes - skipped;
            source += shapeBytes;
        }
        if (total == 0)
        {
            miss();
            return;
        }

        auto staging = Buffer::Create(m_Context, {
                                                     .Name = "GeneratedTextureRestore",
                                                     .Size = total,
                                                     .Usage = BufferUsage::TransferSrc,
                                                     .HostMapped = true,
                                                 });
        job->Restore =
            PendingRestore{.Staging = staging, .TargetOffsets = std::move(targetOffsets)};
        job->Restoring = true;
        UpdateHold(*job);

        // The reads and the copies into the mapping are one worker's work, so a target's levels are
        // never held in a second buffer beside the one they are going to.
        DerivedDataCache* cache = m_Cache;
        const u64 texelOffset = layout->TexelOffset;
        Task<bool> restore = m_Tasks->Submit(
            [cache, cacheKey = job->CacheKey, staging, wanted = std::move(wanted), texelOffset]
            {
                u64 destination = 0;
                for (const Wanted& range : wanted)
                {
                    const optional<vector<u8>> bytes =
                        cache->ReadRange(cacheKey, texelOffset + range.Source, range.Bytes);
                    if (!bytes.has_value() || bytes->size() != range.Bytes)
                    {
                        return false;
                    }
                    staging->UploadSync(*bytes, destination);
                    destination += range.Bytes;
                }
                return true;
            },
            "GeneratedTextureCacheRestore");
        restore.Then(
            [this, key, serial](Result<bool> read)
            {
                if (read.has_value() && *read)
                {
                    ResolveRestore(key, serial);
                    return;
                }
                // The entry went, or came back short, between the header and the levels. The job
                // falls back to running its ticks, which is what a miss would have done.
                Job* pending = FindJob(key);
                if (pending == nullptr || pending->Serial != serial || !pending->Restoring)
                {
                    return;
                }
                pending->Restoring = false;
                pending->Restore.reset();
                UpdateHold(*pending);
            });
    }

    void GeneratedTextureService::ResolveRestore(const GeneratedTextureKey key, const u64 serial)
    {
        Job* job = FindJob(key);
        if (job == nullptr || job->Serial != serial || !job->Restoring)
        {
            return;
        }
        // The hold is deliberately not re-derived: the job's ticks would overwrite the texels the
        // next pump is about to copy in, so it stays unselectable right through to resident.
        job->Restoring = false;
        job->Restore->Staged = true;
    }

    void GeneratedTextureService::ApplyRestores(CommandBuffer& cmd,
                                                vector<GeneratedTextureKey>& restored)
    {
        for (const Unique<Job>& job : m_Jobs)
        {
            if (!job->Restore.has_value() || !job->Restore->Staged)
            {
                continue;
            }
            for (usize i = 0; i < job->Targets.size(); i++)
            {
                const GeneratedTexture& target = *job->Targets[i];
                const vector<BufferImageCopyRegion> regions =
                    LevelRegions(ShapeOf(*target.GetImage()), job->Restore->TargetOffsets[i]);
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

    void GeneratedTextureService::SubmitStore(CommandBuffer& cmd, const Job& job)
    {
        usize total = 0;
        for (const Unique<GeneratedTexture>& target : job.Targets)
        {
            total += GeneratedTextureShapeBytes(ShapeOf(*target->GetImage()));
        }
        if (total == 0)
        {
            return;
        }

        // One buffer for the whole target set, not one per subresource: the levels of a target are
        // contiguous within it, which is the layout a per-mip all-layers copy already writes.
        PendingStore store{
            .CacheKey = job.CacheKey,
            .Staging = Buffer::Create(m_Context,
                                      {
                                          .Name = fmt::format("{} (Cache)", job.CacheKey),
                                          .Size = total,
                                          .Usage = BufferUsage::TransferDst,
                                          .HostMapped = true,
                                      }),
            .Bytes = total,
            .StagedPump = m_PumpCount,
        };
        store.Images.reserve(job.Targets.size());

        u64 base = 0;
        for (const Unique<GeneratedTexture>& target : job.Targets)
        {
            const GeneratedTextureBlobShape shape = ShapeOf(*target->GetImage());
            const vector<BufferImageCopyRegion> regions = LevelRegions(shape, base);
            cmd.PrepareForAccess(target->GetSampledView(), AccessKind::TransferSrc);
            cmd.CopyImageToBuffer(target->GetImage(), store.Staging, regions);
            cmd.PrepareForAccess(target->GetSampledView(), AccessKind::Sample);
            store.Images.push_back(target->GetImage());
            base += GeneratedTextureShapeBytes(shape);
        }

        m_Stores.push_back(std::move(store));
    }

    void GeneratedTextureService::FlushStores()
    {
        if (m_Stores.empty())
        {
            return;
        }

        // A copy recorded framesInFlight pumps ago rode a frame whose fence AcquireNextFrame has
        // since waited, so the mapped bytes are the finished GPU result.
        const u64 latency = m_Context.GetMaxFramesInFlight();
        vector<PendingStore> ready;
        vector<PendingStore> waiting;
        waiting.reserve(m_Stores.size());
        for (PendingStore& store : m_Stores)
        {
            if (m_PumpCount - store.StagedPump >= latency)
            {
                ready.push_back(std::move(store));
            }
            else
            {
                waiting.push_back(std::move(store));
            }
        }
        m_Stores = std::move(waiting);

        for (PendingStore& store : ready)
        {
            if (m_Cache == nullptr || m_Tasks == nullptr)
            {
                continue;
            }

            vector<GeneratedTextureBlobShape> shapes;
            shapes.reserve(store.Images.size());
            for (const Ref<Image>& image : store.Images)
            {
                shapes.push_back(ShapeOf(*image));
            }

            m_StoredTotal++;
            DerivedDataCache* cache = m_Cache;
            // The encode is a pure byte transform over a payload the size of the target set, so it
            // runs beside the file write rather than on the frame thread. Holding the buffer is
            // what keeps the mapping alive until the worker has read it.
            m_Tasks->Submit(
                [cache, storeKey = std::move(store.CacheKey), shapes = std::move(shapes),
                 staging = std::move(store.Staging), bytes = store.Bytes]
                {
                    vector<u8> payload = BeginGeneratedTextureBlob(shapes, bytes);
                    if (payload.empty())
                    {
                        return;
                    }
                    const auto* mapped = static_cast<const u8*>(staging->GetMappedData());
                    payload.insert(payload.end(), mapped, mapped + bytes);
                    cache->Store(storeKey, payload);
                },
                "GeneratedTextureCacheStore");
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
            if (job->Allocating)
            {
                stats.Allocating++;
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
        m_PumpCount++;

        // Ahead of the early-out: a store outlives the job it read, so a set of jobs that have all
        // gone resident still has bytes to hand over.
        FlushStores();

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
            // A tail job holds part of a chain, so what it could store is less than the entry it read
            // from — it restores and never writes back.
            if (!job->CacheKey.empty() && !job->CacheTail && m_Cache != nullptr &&
                m_Tasks != nullptr)
            {
                SubmitStore(cmd, *job);
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
