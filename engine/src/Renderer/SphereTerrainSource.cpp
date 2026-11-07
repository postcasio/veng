#include <Veng/Renderer/SphereTerrain.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Assert.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "GeneratedTextureBlob.h"
#include "SphereTerrainResidency.h"

namespace Veng::Renderer
{
    namespace
    {
        // The engine's bounds reduction (engine/assets/core/shaders/sphere_terrain/).
        constexpr AssetId BoundsCompId{0xE392243C3FBB9116ULL};

        // Distinguishes a tile's measurement job from its generation job under one source salt.
        constexpr u64 MeasureJobTag = 0x6D65617375726521ULL;

        // The result entries a frame slot's buffer starts with; it doubles when a pump outgrows it.
        constexpr u32 InitialBoundsEntries = 64;

        // Each reduction binds its own entry, so no two dispatches write one bound range; Vulkan
        // caps minStorageBufferOffsetAlignment at 256, so that stride is legal on every device.
        constexpr u64 BoundsEntryStride = 256;

        // Mirrors BoundsPush in sphere_terrain_bounds.comp.slang.
        struct BoundsPush
        {
            uvec2 Quadrant{0u};
            u32 HasParent = 0;
        };

        std::atomic<u64> s_NextSerial{1};

        u64 SplitMix64(u64 x)
        {
            x += 0x9E3779B97F4A7C15ULL;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            return x ^ (x >> 31);
        }

        // Generated-texture keys are context-wide, so a tile's key is mixed with its source's salt.
        GeneratedTextureKey MixKey(const u64 salt, const u64 tile)
        {
            return SplitMix64(salt ^ SplitMix64(tile));
        }

        u32 PinnedTileCount(const u32 pinnedLevel)
        {
            u32 count = 0;
            for (u32 level = 0; level <= pinnedLevel; level++)
            {
                count += CubeSphere::FaceCount << (2 * level);
            }
            return count;
        }
    }

    SphereTerrainTileConstants PackSphereTerrainTileConstants(const CubeSphereTileKey key,
                                                              const f64 radius)
    {
        return PackSphereTerrainTileConstants(key, GetCubeSphereTileGeometry(key), radius);
    }

    SphereTerrainTileConstants
    PackSphereTerrainTileConstants(const CubeSphereTileKey key,
                                   const CubeSphereTileGeometry& geometry, const f64 radius)
    {
        const CubeSphereTileOffsetInputs inputs = GetCubeSphereTileOffsetInputs(geometry);
        const vec3 high = vec3(geometry.Center);
        const vec3 low = vec3(geometry.Center - dvec3(high));
        SphereTerrainTileConstants constants{
            .CenterHigh = vec4(high, static_cast<f32>(radius)),
            .CenterLow = vec4(low, 0.0f),
            .Warp = inputs.Warp,
            .Tile = uvec4(static_cast<u32>(key.GetFace()) | (key.GetLevel() << 8), key.GetX(),
                          key.GetY(),
                          SphereTerrainTileTexels | (SphereTerrainTileBorder << 8) |
                              (SphereTerrainTileCells << 16)),
        };
        for (u32 i = 0; i < 4; i++)
        {
            constants.Corners[i] = vec4(inputs.CornerOffsets[i], inputs.CornerNorms[i]);
        }
        return constants;
    }

    struct SphereTerrainSource::Impl
    {
        /// @brief One frame slot's reduction bindings and result buffer.
        struct SlotRing
        {
            /// @brief Descriptor sets, reused once this slot's frame has retired.
            vector<Ref<DescriptorSet>> Sets;
            /// @brief The next set this slot's pump takes.
            u32 SetCursor = 0;
            /// @brief The host-mapped buffer the reductions write into.
            Ref<Buffer> Results;
            /// @brief Its capacity in entries.
            u32 ResultCapacity = 0;
            /// @brief The next entry this slot's pump takes.
            u32 ResultCursor = 0;
        };

        /// @brief A reduction recorded and not yet read back.
        struct PendingMeasure
        {
            CubeSphereTileKey Key;
            u32 Generation = 0;
            Ref<Buffer> Results;
            u32 Index = 0;
            u64 ReadyAt = 0;
        };

        /// @brief A tile with a live generated-texture job, generating or measuring.
        struct TileJob
        {
            GeneratedTextureKey JobKey = 0;
            u32 Generation = 0;
            bool Ticked = false;
            bool Measuring = false;
        };

        Impl(Context& context, const SphereTerrainSourceInfo& info)
            : Ctx(context), Info(info), Serial(s_NextSerial.fetch_add(1)),
              Residency(info.MaxResidentTiles, info.PinnedLevel, info.Radius)
        {
        }

        Context& Ctx;
        SphereTerrainSourceInfo Info;
        vector<SphereTerrainChannelInfo> Channels;
        u64 Serial = 0;
        string CacheSalt;
        vector<Ref<Image>> Images;
        vector<Ref<ImageView>> ArrayViews;
        vector<vector<Ref<ImageView>>> LayerViews;
        SphereTerrainResidency Residency;
        Ref<DescriptorSetLayout> BoundsSetLayout;
        Ref<PipelineLayout> BoundsLayout;
        Ref<ComputePipeline> BoundsPipeline;
        Ref<Image> Placeholder;
        Ref<ImageView> PlaceholderView;
        vector<SlotRing> Slots;
        vector<PendingMeasure> Measures;
        std::unordered_map<u64, TileJob> Jobs;
        vector<GeneratedTextureKey> ToRelease;
        Context::FrameRetiredHandle Retired = 0;
        bool PinnedTopResident = false;
        u64 ViewGeneration = 0;
        u64 Requests = 0;
        u64 Throttled = 0;
        u32 GeneratedThisFrame = 0;
        u32 RestoredThisFrame = 0;
        u32 GeneratedLastFrame = 0;
        u32 RestoredLastFrame = 0;
        u64 GeneratedTotal = 0;
        u64 RestoredTotal = 0;

        [[nodiscard]] GeneratedTextureService& Service() const
        {
            return Ctx.GetGeneratedTextures();
        }

        [[nodiscard]] GeneratedTextureKey TileJobKey(const CubeSphereTileKey key) const
        {
            return MixKey(Serial, key.Value);
        }

        [[nodiscard]] GeneratedTextureKey MeasureJobKey(const CubeSphereTileKey key) const
        {
            return MixKey(Serial ^ MeasureJobTag, key.Value);
        }

        void CreatePool(const u32 capacity)
        {
            ViewGeneration++;
            Images.clear();
            ArrayViews.clear();
            LayerViews.assign(Channels.size(), {});
            for (const SphereTerrainChannelInfo& channel : Channels)
            {
                const u32 texels = SphereTerrainChannelTexels(channel.TexelDivisor);
                const Ref<Image> image = Image::Create(
                    Ctx, {
                             .Name = fmt::format("{} {}", Info.Name, channel.Name),
                             .Extent = {texels, texels, 1},
                             .Layers = capacity,
                             .Format = channel.TexelFormat,
                             .Usage = ImageUsage::Storage | ImageUsage::Sampled |
                                      ImageUsage::TransferSrc | ImageUsage::TransferDst,
                         });
                const Ref<ImageView> view = ImageView::Create(
                    Ctx, {
                             .Name = fmt::format("{} {} (Array)", Info.Name, channel.Name),
                             .Image = image,
                             .ViewType = ImageViewType::Array2D,
                             .ArrayLayers = capacity,
                         });
                Images.push_back(image);
                ArrayViews.push_back(view);
            }
        }

        [[nodiscard]] const Ref<ImageView>& GetLayerView(const u32 channel, const u32 layer)
        {
            vector<Ref<ImageView>>& views = LayerViews[channel];
            if (views.size() <= layer)
            {
                views.resize(Residency.GetCapacity());
            }
            if (!views[layer])
            {
                views[layer] =
                    ImageView::Create(Ctx, {
                                               .Name = fmt::format("{} {} (Layer {})", Info.Name,
                                                                   Channels[channel].Name, layer),
                                               .Image = Images[channel],
                                               .ViewType = ImageViewType::Type2D,
                                               .BaseArrayLayer = layer,
                                           });
            }
            return views[layer];
        }

        void ReleaseJob(const GeneratedTextureKey key)
        {
            Service().Release(key);
            std::erase(ToRelease, key);
        }

        // Forgets a tile the residency core no longer holds.
        void Forget(const CubeSphereTileKey key)
        {
            if (const auto it = Jobs.find(key.Value); it != Jobs.end())
            {
                ReleaseJob(it->second.JobKey);
                Jobs.erase(it);
            }
        }

        void RecordBounds(CommandBuffer& cmd, const CubeSphereTileKey key, const u32 generation,
                          const Ref<ImageView>& height)
        {
            const u32 slotIndex = Ctx.GetCurrentFrameInFlight();
            SlotRing& slot = Slots[slotIndex];
            if (slot.ResultCursor >= slot.ResultCapacity)
            {
                // Entries already recorded this frame keep the buffer they were written to alive
                // through their pending measure.
                slot.ResultCapacity = std::max(InitialBoundsEntries, slot.ResultCapacity * 2);
                slot.Results =
                    Buffer::Create(Ctx, {
                                            .Name = fmt::format("{} Bounds", Info.Name),
                                            .Size = u64{slot.ResultCapacity} * BoundsEntryStride,
                                            .Usage = BufferUsage::Storage,
                                            .HostMapped = true,
                                        });
                slot.ResultCursor = 0;
            }
            if (slot.SetCursor >= slot.Sets.size())
            {
                slot.Sets.push_back(
                    DescriptorSet::Create(Ctx, {.Name = fmt::format("{} Bounds Set", Info.Name),
                                                .Layout = BoundsSetLayout}));
            }
            const Ref<DescriptorSet>& set = slot.Sets[slot.SetCursor++];
            const u32 index = slot.ResultCursor++;

            const bool hasParent = key.GetLevel() > 0;
            Ref<ImageView> parent = PlaceholderView;
            if (hasParent)
            {
                const SphereTerrainTileRecord* record = Residency.Find(key.GetParent());
                VE_ASSERT(record != nullptr,
                          "sphere terrain: a measured tile's parent is not held");
                parent = GetLayerView(0, record->Layer);
            }

            // The reduction reads what the generator just wrote.
            cmd.PrepareForAccess(height, AccessKind::StorageRead);
            set->Write(0, height);
            set->Write(1, parent);
            set->Write(2, slot.Results, u64{index} * BoundsEntryStride, sizeof(vec4));
            cmd.BindPipeline(BoundsPipeline);
            cmd.BindDescriptorSets({set.get()}, BindlessRegistry::FirstUserSet,
                                   PipelineBindPoint::Compute);
            cmd.PushConstants(BoundsPush{
                .Quadrant = uvec2(key.GetX() & 1u, key.GetY() & 1u),
                .HasParent = hasParent ? 1u : 0u,
            });
            cmd.Dispatch(1, 1, 1);
            Measures.push_back({
                .Key = key,
                .Generation = generation,
                .Results = slot.Results,
                .Index = index,
                .ReadyAt = Ctx.GetFrameSerial() + Ctx.GetMaxFramesInFlight(),
            });
        }

        void RecordTile(CommandBuffer& cmd, const GeneratedTextureTickContext& context,
                        const CubeSphereTileKey key, const u32 generation)
        {
            const SphereTerrainTileRecord* record = Residency.Find(key);
            if (record == nullptr || record->Generation != generation)
            {
                return;
            }
            vector<Ref<ImageView>> channelViews;
            channelViews.reserve(context.Targets.size());
            for (const Unique<GeneratedTexture>& target : context.Targets)
            {
                channelViews.push_back(target->GetView(0, 0));
            }
            vector<Ref<ImageView>> parentViews;
            u32 parentLayer = SphereTerrainNoLayer;
            if (key.GetLevel() > 0)
            {
                const SphereTerrainTileRecord* parent = Residency.Find(key.GetParent());
                VE_ASSERT(parent != nullptr,
                          "sphere terrain: a generating tile's parent is not held");
                parentLayer = parent->Layer;
                for (u32 channel = 0; channel < Channels.size(); channel++)
                {
                    parentViews.push_back(GetLayerView(channel, parentLayer));
                }
            }

            const SphereTerrainTileWork work{
                .Key = key,
                .Layer = record->Layer,
                .Geometry = record->Facts.Geometry,
                .Radius = Info.Radius,
                .Constants = record->Facts.Constants,
                .ChannelViews = channelViews,
                .ParentLayer = parentLayer,
                .ParentViews = parentViews,
            };
            Info.Generator->RecordTile(cmd, work);
            RecordBounds(cmd, key, generation, channelViews[0]);
            if (const auto it = Jobs.find(key.Value); it != Jobs.end())
            {
                it->second.Ticked = true;
            }
            GeneratedThisFrame++;
            GeneratedTotal++;
        }

        void SubmitMeasure(const CubeSphereTileKey key, const u32 generation, const u32 layer)
        {
            const GeneratedTextureKey jobKey = MeasureJobKey(key);
            ReleaseJob(jobKey);
            Jobs[key.Value] = {.JobKey = jobKey, .Generation = generation, .Measuring = true};
            Service().Request({
                .Key = jobKey,
                .Name = Info.Name + " Bounds",
                .Targets = {{
                    .Adopt = Images[0],
                    .AdoptBaseLayer = layer,
                    .AdoptLayerCount = 1,
                    .ProducerAccess = AccessKind::StorageRead,
                }},
                .Priority = MeasurePriority,
                .OnTick = [this, key, generation](CommandBuffer& cmd,
                                                  const GeneratedTextureTickContext& context)
                { RecordBounds(cmd, key, generation, context.Targets[0]->GetView(0, 0)); },
                .OnComplete =
                    [this, key, jobKey](const GeneratedTextureResult&)
                {
                    ToRelease.push_back(jobKey);
                    if (const auto it = Jobs.find(key.Value);
                        it != Jobs.end() && it->second.JobKey == jobKey)
                    {
                        Jobs.erase(it);
                    }
                },
            });
        }

        void OnTileComplete(const CubeSphereTileKey key, const u32 generation,
                            const GeneratedTextureKey jobKey)
        {
            ToRelease.push_back(jobKey);
            if (!Residency.Complete(key, generation))
            {
                return;
            }
            const auto it = Jobs.find(key.Value);
            const bool ticked = it != Jobs.end() && it->second.Ticked;
            if (it != Jobs.end())
            {
                Jobs.erase(it);
            }
            if (!ticked)
            {
                // Restored from the cache: no tick ran, so nothing measured it.
                RestoredThisFrame++;
                RestoredTotal++;
                SubmitMeasure(key, generation, Residency.Find(key)->Layer);
            }
        }

        void SubmitTile(const CubeSphereTileKey key, const u32 layer, const u32 generation,
                        const i32 priority)
        {
            const GeneratedTextureKey jobKey = TileJobKey(key);
            ReleaseJob(jobKey);
            Jobs[key.Value] = {.JobKey = jobKey, .Generation = generation};

            GeneratedTextureRequest request{
                .Key = jobKey,
                .Name = Info.Name + " Tile",
                .CacheKey = CacheSalt.empty() ? string{}
                                              : fmt::format("sphere-terrain:{}:{}:{:016X}",
                                                            CacheSalt, Info.Radius, key.Value),
                .Cost = Info.Generator->GetTileCost(key.GetLevel()),
                .Priority = priority,
                .OnTick = [this, key, generation](CommandBuffer& cmd,
                                                  const GeneratedTextureTickContext& context)
                { RecordTile(cmd, context, key, generation); },
                .OnComplete = [this, key, generation, jobKey](const GeneratedTextureResult&)
                { OnTileComplete(key, generation, jobKey); },
            };
            request.Targets.reserve(Images.size());
            for (const Ref<Image>& image : Images)
            {
                request.Targets.push_back({
                    .Adopt = image,
                    .AdoptBaseLayer = layer,
                    .AdoptLayerCount = 1,
                    .ProducerAccess = AccessKind::StorageWrite,
                });
            }
            Service().Request(std::move(request));
            Requests++;
        }

        bool RequestTile(const CubeSphereTileKey key, const i32 priority)
        {
            if (!key.IsValid() || key.GetLevel() > Info.MaxLevel)
            {
                return false;
            }
            if (const SphereTerrainTileRecord* held = Residency.Find(key); held != nullptr)
            {
                Residency.MarkWanted(key);
                if (held->State == SphereTerrainTileState::Pending)
                {
                    if (const auto it = Jobs.find(key.Value);
                        it != Jobs.end() && !it->second.Measuring)
                    {
                        Service().SetPriority(it->second.JobKey, priority);
                    }
                }
                return true;
            }
            if (key.GetLevel() > Info.PinnedLevel &&
                Residency.GetPendingCount() >= Info.MaxPendingTiles)
            {
                Throttled++;
                return false;
            }
            const SphereTerrainResidency::RequestResult result = Residency.Request(key);
            if (result.Outcome != SphereTerrainResidency::RequestOutcome::Accepted)
            {
                return false;
            }
            if (result.Evicted.IsValid())
            {
                Forget(result.Evicted);
            }
            std::array<u32, MaxSphereTerrainTileWords> words{};
            Info.Generator->WriteTileDrawData(key, words);
            Residency.SetTileWords(key, words);
            SubmitTile(key, result.Layer, result.Generation, priority);
            return true;
        }

        void RequestPinnedTop()
        {
            if (PinnedTopResident)
            {
                return;
            }
            bool all = true;
            for (u32 level = 0; level <= Info.PinnedLevel; level++)
            {
                const u32 n = 1u << level;
                for (u32 face = 0; face < CubeSphere::FaceCount; face++)
                {
                    for (u32 y = 0; y < n; y++)
                    {
                        for (u32 x = 0; x < n; x++)
                        {
                            const CubeSphereTileKey key = CubeSphereTileKey::Make(
                                static_cast<CubeSphere::Face>(face), level, x, y);
                            all = all && Residency.IsResident(key);
                            RequestTile(key, PinnedPriority - static_cast<i32>(level));
                        }
                    }
                }
            }
            PinnedTopResident = all;
        }

        void OnFrameRetired(const u32 slotIndex)
        {
            const u64 frame = Ctx.GetFrameSerial();
            Residency.BeginFrame(frame);

            for (const GeneratedTextureKey key : ToRelease)
            {
                Service().Release(key);
            }
            ToRelease.clear();

            std::erase_if(Measures,
                          [&](const PendingMeasure& measure)
                          {
                              if (measure.ReadyAt > frame)
                              {
                                  return false;
                              }
                              vec4 result{0.0f};
                              std::memcpy(&result,
                                          static_cast<const u8*>(measure.Results->GetMappedData()) +
                                              (u64{measure.Index} * BoundsEntryStride),
                                          sizeof(vec4));
                              if (result.w > 0.0f)
                              {
                                  Residency.SetBound(measure.Key, measure.Generation,
                                                     {.Measured = true,
                                                      .Min = result.x,
                                                      .Max = result.y,
                                                      .MorphError = result.z});
                              }
                              return true;
                          });
            SlotRing& slot = Slots[slotIndex];
            slot.SetCursor = 0;
            slot.ResultCursor = 0;

            for (const CubeSphereTileKey key : Residency.CollectStalePending())
            {
                Forget(key);
                Residency.Remove(key);
            }

            RequestPinnedTop();

            GeneratedLastFrame = GeneratedThisFrame;
            RestoredLastFrame = RestoredThisFrame;
            GeneratedThisFrame = 0;
            RestoredThisFrame = 0;
            VE_PROFILE_COUNTER("Render/SphereTerrainResident",
                               static_cast<f64>(Residency.GetResidentCount()));
            VE_PROFILE_COUNTER("Render/SphereTerrainGenerated",
                               static_cast<f64>(GeneratedLastFrame));
        }

        void SetCapacity(u32 capacity)
        {
            capacity = std::min(capacity, Ctx.GetMaxImageArrayLayers());
            for (const auto& [value, job] : Jobs)
            {
                ReleaseJob(job.JobKey);
                const CubeSphereTileKey key = CubeSphereTileKey::FromValue(value);
                if (const SphereTerrainTileRecord* record = Residency.Find(key);
                    record != nullptr && record->State == SphereTerrainTileState::Pending)
                {
                    Residency.Remove(key);
                }
            }
            Jobs.clear();
            PinnedTopResident = false;

            const SphereTerrainResidency::CapacityChange change = Residency.SetCapacity(capacity);
            std::unordered_map<u32, u32> movedFrom;
            for (const auto& [from, to] : change.Moves)
            {
                movedFrom[to] = from;
            }
            vector<ImageLayerCopy> copies;
            for (const auto& [value, record] : Residency.GetTiles())
            {
                const auto moved = movedFrom.find(record.Layer);
                copies.push_back({
                    .SourceLayer = moved != movedFrom.end() ? moved->second : record.Layer,
                    .DestinationLayer = record.Layer,
                });
            }

            const vector<Ref<Image>> oldImages = Images;
            const vector<Ref<ImageView>> oldViews = ArrayViews;
            CreatePool(change.Capacity);
            Ctx.RecordSetupCommands(
                [oldImages, oldViews, newImages = Images, newViews = ArrayViews,
                 copies = std::move(copies)](CommandBuffer& cmd)
                {
                    for (usize channel = 0; channel < newImages.size(); channel++)
                    {
                        cmd.PrepareForAccess(oldViews[channel], AccessKind::TransferSrc);
                        cmd.PrepareForAccess(newViews[channel], AccessKind::TransferDst);
                        cmd.CopyImageLayers(oldImages[channel], newImages[channel], copies);
                        cmd.PrepareForAccess(newViews[channel], AccessKind::SampleAny);
                        cmd.PrepareForAccess(oldViews[channel], AccessKind::SampleAny);
                    }
                });

            for (const auto& [value, record] : Residency.GetTiles())
            {
                if (!record.Bound.Measured)
                {
                    SubmitMeasure(record.Key, record.Generation, record.Layer);
                }
            }
        }
    };

    Ref<SphereTerrainSource> SphereTerrainSource::Create(Context& context, AssetManager& assets,
                                                         const SphereTerrainSourceInfo& info)
    {
        VE_ASSERT(info.Generator != nullptr, "sphere terrain '{}' has no generator", info.Name);
        VE_ASSERT(info.MaxLevel <= CubeSphere::MaxLevel && info.PinnedLevel <= info.MaxLevel,
                  "sphere terrain '{}': pinned level {} and max level {} out of range", info.Name,
                  info.PinnedLevel, info.MaxLevel);
        VE_ASSERT(info.Radius > 0.0 && info.MaxHeight >= info.MinHeight,
                  "sphere terrain '{}': bad radius or height range", info.Name);

        SphereTerrainSourceInfo resolved = info;
        const u32 deviceLayers = context.GetMaxImageArrayLayers();
        if (resolved.MaxResidentTiles > deviceLayers)
        {
            static std::atomic<bool> s_Warned{false};
            if (!s_Warned.exchange(true))
            {
                Log::Warn("sphere terrain '{}': {} resident tiles exceed the device's {} array "
                          "layers; the pool is clamped",
                          info.Name, resolved.MaxResidentTiles, deviceLayers);
            }
            resolved.MaxResidentTiles = deviceLayers;
        }
        VE_ASSERT(resolved.MaxResidentTiles > PinnedTileCount(resolved.PinnedLevel),
                  "sphere terrain '{}': {} tiles cannot hold the {} pinned ones", info.Name,
                  resolved.MaxResidentTiles, PinnedTileCount(resolved.PinnedLevel));

        auto impl = CreateUnique<Impl>(context, resolved);
        impl->Channels = resolved.Generator->GetChannels();
        VE_ASSERT(!impl->Channels.empty() && impl->Channels.size() <= MaxSphereTerrainChannels,
                  "sphere terrain '{}' declares {} channels (1 to {})", info.Name,
                  impl->Channels.size(), MaxSphereTerrainChannels);
        VE_ASSERT(impl->Channels[0].TexelFormat == Format::R32Sfloat &&
                      impl->Channels[0].TexelDivisor == 1,
                  "sphere terrain '{}': channel 0 is height, R32Sfloat at divisor 1", info.Name);
        for (const SphereTerrainChannelInfo& channel : impl->Channels)
        {
            VE_ASSERT(channel.TexelDivisor > 0 &&
                          (SphereTerrainTileTexels - 1) % channel.TexelDivisor == 0,
                      "sphere terrain '{}': channel '{}' divisor {} does not divide {}", info.Name,
                      channel.Name, channel.TexelDivisor, SphereTerrainTileTexels - 1);
        }
        impl->CacheSalt = resolved.Generator->GetCacheSalt();

        const AssetResult<AssetHandle<Veng::Shader>> shader =
            assets.LoadSync<Veng::Shader>(BoundsCompId);
        VE_ASSERT(shader.has_value(), "sphere terrain: bounds shader load failed: {}",
                  shader.error().Detail);
        impl->BoundsSetLayout =
            DescriptorSetLayout::Create(context, {
                                                     .Name = "Sphere Terrain Bounds Set Layout",
                                                     .Bindings =
                                                         {
                                                             {.Binding = 0,
                                                              .Type = DescriptorType::StorageImage,
                                                              .Count = 1,
                                                              .Stages = ShaderStage::Compute},
                                                             {.Binding = 1,
                                                              .Type = DescriptorType::SampledImage,
                                                              .Count = 1,
                                                              .Stages = ShaderStage::Compute},
                                                             {.Binding = 2,
                                                              .Type = DescriptorType::StorageBuffer,
                                                              .Count = 1,
                                                              .Stages = ShaderStage::Compute},
                                                         },
                                                 });
        impl->BoundsLayout = PipelineLayout::Create(
            context,
            {
                .Name = "Sphere Terrain Bounds Layout",
                .DescriptorSetLayouts = {impl->BoundsSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<BoundsPush>(ShaderStage::Compute)},
            });
        impl->BoundsPipeline = ComputePipeline::Create(
            context,
            {
                .Name = "Sphere Terrain Bounds",
                .PipelineLayout = impl->BoundsLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Get()->Module},
            });

        impl->Placeholder = Image::Create(context, {
                                                       .Name = "Sphere Terrain Bounds Placeholder",
                                                       .Extent = {1, 1, 1},
                                                       .Format = Format::R32Sfloat,
                                                       .Usage = ImageUsage::Sampled,
                                                   });
        impl->PlaceholderView = ImageView::Create(
            context, {.Name = "Sphere Terrain Bounds Placeholder", .Image = impl->Placeholder});
        impl->Slots.resize(context.GetMaxFramesInFlight());
        impl->CreatePool(resolved.MaxResidentTiles);

        // Every layer is held sampleable outside its own tick, so a whole-array binding is legal
        // from the first frame; the placeholder only ever stands in for a level-0 tile's parent.
        context.RecordSetupCommands(
            [views = impl->ArrayViews, placeholder = impl->PlaceholderView](CommandBuffer& cmd)
            {
                for (const Ref<ImageView>& view : views)
                {
                    cmd.PrepareForAccess(view, AccessKind::SampleAny);
                }
                cmd.PrepareForAccess(placeholder, AccessKind::SampleAny);
            });

        Impl* raw = impl.get();
        impl->Retired =
            context.AddFrameRetiredCallback([raw](const u32 slot) { raw->OnFrameRetired(slot); });
        raw->RequestPinnedTop();
        return Ref<SphereTerrainSource>(new SphereTerrainSource(std::move(impl)));
    }

    SphereTerrainSource::SphereTerrainSource(Unique<Impl> impl) : m_Impl(std::move(impl)) {}

    SphereTerrainSource::~SphereTerrainSource()
    {
        m_Impl->Ctx.RemoveFrameRetiredCallback(m_Impl->Retired);
        for (const auto& [value, job] : m_Impl->Jobs)
        {
            m_Impl->Service().Release(job.JobKey);
        }
        for (const GeneratedTextureKey key : m_Impl->ToRelease)
        {
            m_Impl->Service().Release(key);
        }
    }

    u64 SphereTerrainSource::GetSerial() const
    {
        return m_Impl->Serial;
    }

    u64 SphereTerrainSource::GetViewGeneration() const
    {
        return m_Impl->ViewGeneration;
    }

    const string& SphereTerrainSource::GetName() const
    {
        return m_Impl->Info.Name;
    }

    f64 SphereTerrainSource::GetRadius() const
    {
        return m_Impl->Info.Radius;
    }

    f32 SphereTerrainSource::GetMinHeight() const
    {
        return m_Impl->Info.MinHeight;
    }

    f32 SphereTerrainSource::GetMaxHeight() const
    {
        return m_Impl->Info.MaxHeight;
    }

    u32 SphereTerrainSource::GetPinnedLevel() const
    {
        return m_Impl->Info.PinnedLevel;
    }

    u32 SphereTerrainSource::GetMaxLevel() const
    {
        return m_Impl->Info.MaxLevel;
    }

    u32 SphereTerrainSource::GetCapacity() const
    {
        return m_Impl->Residency.GetCapacity();
    }

    SphereTerrainGenerator& SphereTerrainSource::GetGenerator() const
    {
        return *m_Impl->Info.Generator;
    }

    u32 SphereTerrainSource::GetChannelCount() const
    {
        return static_cast<u32>(m_Impl->Channels.size());
    }

    const SphereTerrainChannelInfo& SphereTerrainSource::GetChannel(const u32 channel) const
    {
        return m_Impl->Channels.at(channel);
    }

    const Ref<Image>& SphereTerrainSource::GetChannelImage(const u32 channel) const
    {
        return m_Impl->Images.at(channel);
    }

    const Ref<ImageView>& SphereTerrainSource::GetChannelArrayView(const u32 channel) const
    {
        return m_Impl->ArrayViews.at(channel);
    }

    void SphereTerrainSource::SetCapacity(const u32 capacity)
    {
        m_Impl->SetCapacity(capacity);
    }

    bool SphereTerrainSource::IsResident(const CubeSphereTileKey key) const
    {
        return m_Impl->Residency.IsResident(key);
    }

    bool SphereTerrainSource::IsPending(const CubeSphereTileKey key) const
    {
        const SphereTerrainTileRecord* record = m_Impl->Residency.Find(key);
        return record != nullptr && record->State == SphereTerrainTileState::Pending;
    }

    bool SphereTerrainSource::IsPinnedTopResident() const
    {
        return m_Impl->Residency.GetPinnedResidentCount() ==
               PinnedTileCount(m_Impl->Info.PinnedLevel);
    }

    optional<SphereTerrainResidentTile>
    SphereTerrainSource::FindResident(const CubeSphereTileKey key) const
    {
        const SphereTerrainTileRecord* record = m_Impl->Residency.Find(key);
        if (record == nullptr || record->State != SphereTerrainTileState::Resident)
        {
            return std::nullopt;
        }
        return SphereTerrainResidentTile{
            .Key = key,
            .Layer = record->Layer,
            .Measured = record->Bound.Measured,
            .MinHeight = record->Bound.Min,
            .MaxHeight = record->Bound.Max,
            .MorphError = record->Bound.MorphError,
        };
    }

    SphereTerrainResidentTile SphereTerrainSource::FindDeepestResident(const dvec3& direction) const
    {
        const CubeSphere::FacePoint point = CubeSphere::DirectionToFace(direction);
        SphereTerrainResidentTile deepest;
        for (u32 level = 0; level <= m_Impl->Info.MaxLevel; level++)
        {
            const u32 n = 1u << level;
            const auto cell = [n](const f64 coordinate)
            {
                const f64 scaled = std::floor((coordinate + 1.0) * 0.5 * static_cast<f64>(n));
                return static_cast<u32>(std::clamp(scaled, 0.0, static_cast<f64>(n - 1)));
            };
            const CubeSphereTileKey key =
                CubeSphereTileKey::Make(point.OnFace, level, cell(point.U), cell(point.V));
            const optional<SphereTerrainResidentTile> tile = FindResident(key);
            if (!tile.has_value())
            {
                break;
            }
            deepest = *tile;
        }
        return deepest;
    }

    void SphereTerrainSource::Want(const CubeSphereTileKey key)
    {
        m_Impl->Residency.MarkWanted(key);
    }

    bool SphereTerrainSource::Request(const CubeSphereTileKey key, const i32 priority)
    {
        return m_Impl->RequestTile(key, priority);
    }

    SphereTerrainSourceStats SphereTerrainSource::GetStats() const
    {
        const Impl& impl = *m_Impl;
        u64 bytes = 0;
        for (const Ref<Image>& image : impl.Images)
        {
            bytes += GeneratedTextureShapeBytes({
                .TexelFormat = image->GetFormat(),
                .Type = image->GetType(),
                .Extent = image->GetExtent(),
                .Layers = image->GetLayers(),
                .MipLevels = 1,
            });
        }
        return {
            .ResidentTiles = impl.Residency.GetResidentCount(),
            .PinnedTiles = impl.Residency.GetPinnedResidentCount(),
            .PendingTiles = impl.Residency.GetPendingCount(),
            .Capacity = impl.Residency.GetCapacity(),
            .Bytes = bytes,
            .Evictions = impl.Residency.GetEvictions(),
            .Requests = impl.Requests,
            .RequestsDeferred = impl.Residency.GetDeferred() + impl.Throttled,
            .TilesGeneratedLastFrame = impl.GeneratedLastFrame,
            .TilesRestoredLastFrame = impl.RestoredLastFrame,
            .TilesGeneratedTotal = impl.GeneratedTotal,
            .TilesRestoredTotal = impl.RestoredTotal,
        };
    }

    const SphereTerrainResidency& SphereTerrainSource::GetResidency() const
    {
        return m_Impl->Residency;
    }
}
