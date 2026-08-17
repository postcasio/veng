#include <Veng/Renderer/BindlessRegistry.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    namespace
    {
        // Bit-pattern equality for a sampler's float fields. Two descriptions differing by an
        // epsilon produce two different Vulkan samplers, so the cache must treat them as different
        // too; comparing the bits also makes the relation reflexive for a NaN LOD, which `==` is
        // not — a description carrying one would otherwise miss against itself and take a fresh
        // slot on every ask.
        bool SameBits(f32 a, f32 b)
        {
            return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
        }

        // The tightly-packed bytes of every subresource a view exposes — the mips from its base,
        // each level's depth slices, times its layers. The format's block geometry sizes a level, so
        // a compressed image reports its compressed footprint; a format FormatInfo cannot size
        // contributes zero rather than a guess.
        u64 ViewedBytes(const ImageView& view, const Image& image)
        {
            const uvec3 extent = image.GetExtent();
            const Format format = image.GetFormat();
            u64 bytes = 0;
            for (u32 i = 0; i < view.GetMipLevels(); ++i)
            {
                const u32 level = view.GetBaseMipLevel() + i;
                const u32 width = std::max(1u, extent.x >> level);
                const u32 height = std::max(1u, extent.y >> level);
                const u32 depth = std::max(1u, extent.z >> level);
                bytes += static_cast<u64>(BytesForLevel(format, width, height)) * depth *
                         view.GetArrayLayers();
            }
            return bytes;
        }

        // True when two descriptions ask the GPU for the same sampling. Compared field by field:
        // SamplerInfo carries a string, floats and enumerators of several widths, so a memcmp would
        // read the padding between them and a hash over the object would hash it. Name is a debug
        // label and is deliberately absent — it changes nothing about how the sampler filters.
        bool SameSampling(const SamplerInfo& a, const SamplerInfo& b)
        {
            return a.MagFilter == b.MagFilter && a.MinFilter == b.MinFilter &&
                   a.MipmapMode == b.MipmapMode && a.AddressModeU == b.AddressModeU &&
                   a.AddressModeV == b.AddressModeV && a.AddressModeW == b.AddressModeW &&
                   SameBits(a.MipLodBias, b.MipLodBias) &&
                   a.AnisotropyEnabled == b.AnisotropyEnabled &&
                   SameBits(a.MaxAnisotropy, b.MaxAnisotropy) &&
                   a.CompareEnable == b.CompareEnable && a.CompareOp == b.CompareOp &&
                   SameBits(a.MinLod, b.MinLod) && SameBits(a.MaxLod, b.MaxLod) &&
                   a.BorderColor == b.BorderColor &&
                   a.UnnormalizedCoordinates == b.UnnormalizedCoordinates;
        }
    }

    void BindlessRegistry::SlotArray::Init(u32 capacity, u32 framesInFlight)
    {
        Slots.resize(capacity);
        Free.resize(capacity);
        for (u32 i = 0; i < capacity; i++)
        {
            Free[i] = capacity - 1 - i;
        }
        PendingRelease.resize(framesInFlight);
    }

    u32 BindlessRegistry::SlotArray::Allocate(Ref<void> resource, string_view what)
    {
        VE_ASSERT(!Free.empty(), "BindlessRegistry: {} array exhausted ({} slots)", what,
                  Slots.size());

        const u32 index = Free.back();
        Free.pop_back();
        Slots[index] = std::move(resource);
        return index;
    }

    void BindlessRegistry::SlotArray::ReleaseDeferred(u32 index, u32 currentFrameInFlight)
    {
        PendingRelease[currentFrameInFlight].push_back(index);
    }

    void BindlessRegistry::SlotArray::OnFrameAcquired(u32 frameInFlight)
    {
        for (const u32 index : PendingRelease[frameInFlight])
        {
            Slots[index].reset();
            Free.push_back(index);
        }
        PendingRelease[frameInFlight].clear();
    }

    BindlessRegistry::BindlessRegistry(Context& context) : m_Context(context)
    {
        m_Layout = DescriptorSetLayout::Create(
            context,
            {
                .Name = "Bindless Set 0 Layout",
                .Bindings =
                    {
                        {.Binding = TextureBinding,
                         .Type = DescriptorType::SampledImage,
                         .Count = MaxTextures,
                         .Stages = ShaderStage::All,
                         .Bindless = true},
                        {.Binding = SamplerBinding,
                         .Type = DescriptorType::Sampler,
                         .Count = MaxSamplers,
                         .Stages = ShaderStage::All,
                         .Bindless = true},
                        {.Binding = StorageImageBinding,
                         .Type = DescriptorType::StorageImage,
                         .Count = MaxStorageImages,
                         .Stages = ShaderStage::All,
                         .Bindless = true},
                        // The byte-address storage-buffer array: a `ByteAddressBuffer g_Buffers[]`
                        // on the shader side, each slot a whole game-supplied buffer read at full
                        // range and selected by handle index. A non-dynamic, update-after-bind
                        // array — no dynamic descriptor offset (which mistranslates inside set 0's
                        // Metal argument buffer on MoltenVK), so a uniform-per-draw handle indexes
                        // it exactly as the sampled-image array is indexed.
                        {.Binding = StorageBufferBinding,
                         .Type = DescriptorType::StorageBuffer,
                         .Count = MaxStorageBuffers,
                         .Stages = ShaderStage::All,
                         .Bindless = true},
                        // The per-material block buffer: a single ByteAddressBuffer on the
                        // shader side, byte-addressed at index * MaterialParamStride. A draw
                        // folds the current frame's region base into that index, so the load
                        // lands in this frame's copy of the ring-buffered buffer.
                        {.Binding = MaterialParamBinding,
                         .Type = DescriptorType::StorageBuffer,
                         .Count = 1,
                         .Stages = ShaderStage::All},
                        // The per-frame view-constants buffer: a single ByteAddressBuffer
                        // byte-addressed at index * ViewConstantsStride. A pass pushes the
                        // current frame-in-flight index so the load reads this frame's region.
                        {.Binding = ViewConstantsBinding,
                         .Type = DescriptorType::StorageBuffer,
                         .Count = 1,
                         .Stages = ShaderStage::All},
                        // The per-frame light buffer: a single ByteAddressBuffer byte-addressed
                        // at index * LightStride. A pass folds the current frame's region base
                        // into its per-light index so the load reads this frame's region.
                        {.Binding = LightBinding,
                         .Type = DescriptorType::StorageBuffer,
                         .Count = 1,
                         .Stages = ShaderStage::All},
                        // The per-frame area-light vertex buffer: a single ByteAddressBuffer of
                        // vec4 vertices, byte-addressed at index * AreaVertexStride. Rect and
                        // Polygon lights read their world-space vertices here; the folded view
                        // base selects the current region, like the light buffer above.
                        {.Binding = AreaVertexBinding,
                         .Type = DescriptorType::StorageBuffer,
                         .Count = 1,
                         .Stages = ShaderStage::All},
                    },
            });

        m_Set = DescriptorSet::Create(context, {
                                                   .Name = "Bindless Set 0",
                                                   .Layout = m_Layout,
                                               });

        // The typed sets: one homogeneous arrayed binding each, built exactly as set 0's texture
        // array is. Keeping each array a single view type is what keeps a non-2D descriptor out of
        // set 0's Metal argument buffer — the MoltenVK case the typed sets exist to sidestep.
        m_VolumeLayout = DescriptorSetLayout::Create(
            context, {
                         .Name = "Bindless Volume Set Layout",
                         .Bindings = {{.Binding = TypedSetBinding,
                                       .Type = DescriptorType::SampledImage,
                                       .Count = MaxVolumes,
                                       .Stages = ShaderStage::All,
                                       .Bindless = true}},
                     });
        m_VolumeSet = DescriptorSet::Create(context, {
                                                         .Name = "Bindless Volume Set",
                                                         .Layout = m_VolumeLayout,
                                                     });

        m_CubeLayout = DescriptorSetLayout::Create(
            context, {
                         .Name = "Bindless Cube Set Layout",
                         .Bindings = {{.Binding = TypedSetBinding,
                                       .Type = DescriptorType::SampledImage,
                                       .Count = MaxCubes,
                                       .Stages = ShaderStage::All,
                                       .Bindless = true}},
                     });
        m_CubeSet = DescriptorSet::Create(context, {
                                                       .Name = "Bindless Cube Set",
                                                       .Layout = m_CubeLayout,
                                                   });

        m_FramesInFlight = context.GetMaxFramesInFlight();

        // Ring-buffered by framesInFlight; each frame writes its own region while
        // not yet submitted. Bound at full range — a draw folds the frame base into
        // the pushed material index to select the current frame's region.
        m_MaterialParamBuffer =
            Buffer::Create(context, {
                                        .Name = "Bindless MaterialParams",
                                        .Size = static_cast<u64>(m_FramesInFlight) * MaxMaterials *
                                                MaterialParamStride,
                                        .Usage = BufferUsage::Storage,
                                        .HostMapped = true,
                                    });
        m_Set->Write(MaterialParamBinding, m_MaterialParamBuffer);

        // Ringed by framesInFlight * MaxViewsPerFrame: each viewport render owns a distinct region
        // within the frame (TryBeginView advances the slot), so two viewports in one frame do not
        // clobber.
        m_ViewConstantsBuffer =
            Buffer::Create(context, {
                                        .Name = "Bindless ViewConstants",
                                        .Size = static_cast<u64>(m_FramesInFlight) *
                                                MaxViewsPerFrame * ViewConstantsStride,
                                        .Usage = BufferUsage::Storage,
                                        .HostMapped = true,
                                    });
        m_Set->Write(ViewConstantsBinding, m_ViewConstantsBuffer);

        // Ringed by framesInFlight * MaxViewsPerFrame, like the view constants above.
        m_LightBuffer =
            Buffer::Create(context, {
                                        .Name = "Bindless Lights",
                                        .Size = static_cast<u64>(m_FramesInFlight) *
                                                MaxViewsPerFrame * MaxLights * LightStride,
                                        .Usage = BufferUsage::Storage,
                                        .HostMapped = true,
                                    });
        m_Set->Write(LightBinding, m_LightBuffer);

        // Ringed by framesInFlight * MaxViewsPerFrame, parallel to the light buffer.
        m_AreaVertexBuffer = Buffer::Create(
            context, {
                         .Name = "Bindless AreaVertices",
                         .Size = static_cast<u64>(m_FramesInFlight) * MaxViewsPerFrame *
                                 MaxAreaVertices * AreaVertexStride,
                         .Usage = BufferUsage::Storage,
                         .HostMapped = true,
                     });
        m_Set->Write(AreaVertexBinding, m_AreaVertexBuffer);

        m_Textures.Init(MaxTextures, m_FramesInFlight);
        m_Volumes.Init(MaxVolumes, m_FramesInFlight);
        m_Cubes.Init(MaxCubes, m_FramesInFlight);
        m_Samplers.Init(MaxSamplers, m_FramesInFlight);
        m_StorageImages.Init(MaxStorageImages, m_FramesInFlight);
        m_StorageBuffers.Init(MaxStorageBuffers, m_FramesInFlight);
        m_Materials.Init(MaxMaterials, m_FramesInFlight);
        m_MaterialEntries.resize(MaxMaterials);
    }

    BindlessRegistry::~BindlessRegistry() = default;

    void BindlessRegistry::WriteTexture(u32 index, const Ref<ImageView>& view) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .imageView = GetVkImageView(*view),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_Set),
            .dstBinding = TextureBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::SampledImage),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    void BindlessRegistry::WriteVolume(u32 index, const Ref<ImageView>& view) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .imageView = GetVkImageView(*view),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_VolumeSet),
            .dstBinding = TypedSetBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::SampledImage),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    void BindlessRegistry::WriteCube(u32 index, const Ref<ImageView>& view) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .imageView = GetVkImageView(*view),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_CubeSet),
            .dstBinding = TypedSetBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::SampledImage),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    void BindlessRegistry::WriteSampler(u32 index, const Ref<Sampler>& sampler) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .sampler = GetVkSampler(*sampler),
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_Set),
            .dstBinding = SamplerBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::Sampler),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    void BindlessRegistry::WriteStorageImage(u32 index, const Ref<ImageView>& view) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .imageView = GetVkImageView(*view),
            .imageLayout = vk::ImageLayout::eGeneral,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_Set),
            .dstBinding = StorageImageBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::StorageImage),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    void BindlessRegistry::WriteStorageBuffer(u32 index, const Ref<Buffer>& buffer) const
    {
        // Bind the whole buffer at full range; a material selects the slot by handle index and
        // byte-addresses inside it, so there is no dynamic offset.
        const vk::DescriptorBufferInfo bufferInfo{
            .buffer = GetVkBuffer(*buffer),
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_Set),
            .dstBinding = StorageBufferBinding,
            .dstArrayElement = index,
            .descriptorCount = 1,
            .descriptorType = ToVk(DescriptorType::StorageBuffer),
            .pBufferInfo = &bufferInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});
    }

    TextureHandle BindlessRegistry::Register(const Ref<ImageView>& sampled)
    {
        VE_ASSERT(sampled->GetViewType() == ImageViewType::Type2D,
                  "BindlessRegistry::Register: view '{}' is not Type2D — a 3D view uses "
                  "RegisterVolume and a cube view RegisterCube, so no non-2D descriptor enters "
                  "set 0's argument buffer",
                  sampled->GetName());
        const u32 index = m_Textures.Allocate(sampled, "texture");
        WriteTexture(index, sampled);
        return TextureHandle{index};
    }

    VolumeHandle BindlessRegistry::RegisterVolume(const Ref<ImageView>& volume)
    {
        VE_ASSERT(volume->GetViewType() == ImageViewType::Type3D,
                  "BindlessRegistry::RegisterVolume: view '{}' is not Type3D — the volume set's "
                  "array is uniformly 3D",
                  volume->GetName());
        const u32 index = m_Volumes.Allocate(volume, "volume");
        WriteVolume(index, volume);
        return VolumeHandle{index};
    }

    CubeHandle BindlessRegistry::RegisterCube(const Ref<ImageView>& cube)
    {
        VE_ASSERT(cube->GetViewType() == ImageViewType::Cube,
                  "BindlessRegistry::RegisterCube: view '{}' is not Cube — the cube set's "
                  "array is uniformly cube",
                  cube->GetName());
        const u32 index = m_Cubes.Allocate(cube, "cube");
        WriteCube(index, cube);
        return CubeHandle{index};
    }

    SamplerHandle BindlessRegistry::Register(const Ref<Sampler>& sampler)
    {
        const u32 index = m_Samplers.Allocate(sampler, "sampler");
        WriteSampler(index, sampler);
        return SamplerHandle{index};
    }

    SharedSampler BindlessRegistry::AcquireSampler(const SamplerInfo& info)
    {
        for (const SamplerCacheEntry& entry : m_SharedSamplers)
        {
            if (SameSampling(entry.Info, info))
            {
                return entry.Shared;
            }
        }

        const Ref<Sampler> sampler = Sampler::Create(m_Context, info);
        const u32 index = m_Samplers.Allocate(sampler, "sampler");
        WriteSampler(index, sampler);

        const SharedSampler shared{.Sampler = sampler, .Handle = SamplerHandle{index}};
        m_SharedSamplers.emplace_back(SamplerCacheEntry{.Info = info, .Shared = shared});
        return shared;
    }

    StorageImageHandle BindlessRegistry::RegisterStorage(const Ref<ImageView>& storage)
    {
        const u32 index = m_StorageImages.Allocate(storage, "storage image");
        WriteStorageImage(index, storage);
        return StorageImageHandle{index};
    }

    StorageBufferHandle BindlessRegistry::Register(const Ref<Buffer>& buffer)
    {
        const u32 index = m_StorageBuffers.Allocate(buffer, "storage buffer");
        WriteStorageBuffer(index, buffer);
        return StorageBufferHandle{index};
    }

    MaterialHandle BindlessRegistry::RegisterMaterial(std::span<const std::byte> block)
    {
        const u32 index = m_Materials.Allocate(Ref<void>{}, "material");
        UpdateMaterial(MaterialHandle{index}, block);
        return MaterialHandle{index};
    }

    void BindlessRegistry::UpdateMaterial(MaterialHandle handle, std::span<const std::byte> block)
    {
        VE_ASSERT(handle.IsValid(), "BindlessRegistry::UpdateMaterial: invalid handle");
        VE_ASSERT(block.size() <= MaterialParamStride,
                  "BindlessRegistry::UpdateMaterial: block is {} bytes, exceeds stride {}",
                  block.size(), MaterialParamStride);
        VE_ASSERT(handle.Index < MaxMaterials,
                  "BindlessRegistry::UpdateMaterial: slot {} out of range", handle.Index);

        // Cache the block and mark it dirty for framesInFlight frames so
        // OnFrameAcquired flushes it into every ring region.
        MaterialEntry& entry = m_MaterialEntries[handle.Index];
        const std::span<const u8> blockBytes(reinterpret_cast<const u8*>(block.data()),
                                             block.size());
        entry.Block.assign(blockBytes.begin(), blockBytes.end());
        entry.DirtyFrames = m_FramesInFlight;

        // Also write the current frame's region immediately so a mid-frame update
        // is visible to this frame's draws. The current region is safe to write —
        // it is not yet submitted. This does not consume a dirty count.
        WriteMaterialRegion(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::WriteMaterialRegion(u32 materialIndex, u32 frameInFlight) const
    {
        const MaterialEntry& entry = m_MaterialEntries[materialIndex];
        if (entry.Block.empty())
        {
            return;
        }

        const u64 regionBase = static_cast<u64>(frameInFlight) * MaxMaterials * MaterialParamStride;
        const u64 offset = regionBase + static_cast<u64>(materialIndex) * MaterialParamStride;
        auto* base = static_cast<u8*>(m_MaterialParamBuffer->GetMappedData());
        std::memcpy(base + offset, entry.Block.data(), entry.Block.size());
    }

    void BindlessRegistry::Release(TextureHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_Textures.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(VolumeHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_Volumes.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(CubeHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_Cubes.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(SamplerHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }

        // A shared slot is indexed by every caller that asked for its description, none of which
        // can see the others, so freeing it here would hand a live slot back to the free list and
        // let the next Register overwrite a sampler still being drawn through.
        for (const SamplerCacheEntry& entry : m_SharedSamplers)
        {
            VE_ASSERT(entry.Shared.Handle.Index != handle.Index,
                      "BindlessRegistry::Release: sampler slot {} holds the shared '{}', which the "
                      "registry owns; only a Register(Ref<Sampler>) slot is a caller's to release",
                      handle.Index, entry.Info.Name);
        }

        m_Samplers.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(StorageImageHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_StorageImages.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(StorageBufferHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_StorageBuffers.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(MaterialHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        m_Materials.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    BindlessCapacity BindlessRegistry::GetFreeSlots() const
    {
        return BindlessCapacity{
            .Textures = static_cast<u32>(m_Textures.Free.size()),
            .Volumes = static_cast<u32>(m_Volumes.Free.size()),
            .Cubes = static_cast<u32>(m_Cubes.Free.size()),
            .Samplers = static_cast<u32>(m_Samplers.Free.size()),
            .StorageImages = static_cast<u32>(m_StorageImages.Free.size()),
            .StorageBuffers = static_cast<u32>(m_StorageBuffers.Free.size()),
            .Materials = static_cast<u32>(m_Materials.Free.size()),
        };
    }

    vector<BindlessSlot> BindlessRegistry::DescribeSlots(const BindlessArray array) const
    {
        const SlotArray* const slots = SlotsFor(array);
        VE_ASSERT(slots != nullptr, "BindlessRegistry::DescribeSlots: unmapped BindlessArray {}",
                  static_cast<u32>(array));

        // Occupied is the default because it is the state with no list to read: a slot is occupied
        // exactly when neither the free list nor a pending bucket names it.
        vector<BindlessSlot> described(slots->Slots.size());
        for (u32 index = 0; index < described.size(); ++index)
        {
            described[index].Index = index;
            described[index].State = BindlessSlotState::Occupied;
        }
        for (const u32 index : slots->Free)
        {
            described[index].State = BindlessSlotState::Free;
        }
        for (const vector<u32>& pending : slots->PendingRelease)
        {
            for (const u32 index : pending)
            {
                described[index].State = BindlessSlotState::PendingRelease;
            }
        }

        // A released slot keeps its Ref until its window expires, so it is described exactly as an
        // occupied one — which is the point: what a pending slot still holds is what a reader
        // chasing a slow release wants to see.
        for (BindlessSlot& slot : described)
        {
            if (slot.State == BindlessSlotState::Free)
            {
                continue;
            }
            Describe(array, slots->Slots[slot.Index], slot);
        }
        return described;
    }

    const BindlessRegistry::SlotArray* BindlessRegistry::SlotsFor(const BindlessArray array) const
    {
        switch (array)
        {
        case BindlessArray::Textures:
            return &m_Textures;
        case BindlessArray::Volumes:
            return &m_Volumes;
        case BindlessArray::Cubes:
            return &m_Cubes;
        case BindlessArray::Samplers:
            return &m_Samplers;
        case BindlessArray::StorageImages:
            return &m_StorageImages;
        case BindlessArray::StorageBuffers:
            return &m_StorageBuffers;
        case BindlessArray::Materials:
            return &m_Materials;
        }
        return nullptr;
    }

    void BindlessRegistry::Describe(const BindlessArray array, const Ref<void>& resource,
                                    BindlessSlot& slot) const
    {
        // The material table allocates its slots against an empty Ref — a material is bytes in a
        // buffer region, not a resource — so its description comes from the CPU-side block cache.
        if (array == BindlessArray::Materials)
        {
            slot.SizeBytes = m_MaterialEntries[slot.Index].Block.size();
            return;
        }
        if (!resource)
        {
            return;
        }

        // Each array is homogeneous in the type it was allocated with, so the void slot casts back
        // to the type its own Register took.
        switch (array)
        {
        case BindlessArray::Textures:
        case BindlessArray::Volumes:
        case BindlessArray::Cubes:
        case BindlessArray::StorageImages:
        {
            const Ref<ImageView> view = std::static_pointer_cast<ImageView>(resource);
            slot.Name = view->GetName();
            slot.ImageFormat = view->GetFormat();
            slot.MipLevels = view->GetMipLevels();
            slot.ArrayLayers = view->GetArrayLayers();
            if (const Ref<Image> image = view->GetImage())
            {
                slot.Extent = image->GetExtent();
                slot.ImageBytes = ViewedBytes(*view, *image);
            }
            break;
        }
        case BindlessArray::Samplers:
            slot.Name = std::static_pointer_cast<Sampler>(resource)->GetName();
            break;
        case BindlessArray::StorageBuffers:
        {
            const Ref<Buffer> buffer = std::static_pointer_cast<Buffer>(resource);
            slot.Name = buffer->GetName();
            slot.SizeBytes = buffer->GetSize();
            break;
        }
        case BindlessArray::Materials:
            break;
        }
    }

    void BindlessRegistry::Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint) const
    {
        cmd.BindDescriptorSets({
            .Sets = {m_Set, m_VolumeSet, m_CubeSet},
            .FirstSet = 0,
            .PipelineBindPoint = bindPoint,
        });
    }

    u32 BindlessRegistry::GetCurrentFrameBase() const
    {
        return m_Context.GetCurrentFrameInFlight() * MaxMaterials;
    }

    bool BindlessRegistry::TryBeginView()
    {
        if (m_ViewsThisFrame >= MaxViewsPerFrame)
        {
            // A budget reached by ordinary content, so the frame degrades instead of aborting: the
            // refused view records nothing and the slot the last grant handed out stays current, so
            // no write lands in a region another view's draws read at submit.
            if (!m_ViewBudgetWarned)
            {
                m_ViewBudgetWarned = true;
                Log::Warn(
                    "BindlessRegistry: this frame wants more than the {} view slots one frame "
                    "holds; the views past that render nothing. Scene captures give way "
                    "before presented viewports do.",
                    MaxViewsPerFrame);
            }
            return false;
        }
        m_ViewSlot = m_ViewsThisFrame;
        ++m_ViewsThisFrame;
        return true;
    }

    u32 BindlessRegistry::GetRemainingViews() const
    {
        // TryBeginView is the only path that advances the counter and it never grants past the
        // budget, so the subtraction cannot wrap.
        return MaxViewsPerFrame - m_ViewsThisFrame;
    }

    void BindlessRegistry::WriteViewConstants(std::span<const std::byte> block)
    {
        VE_ASSERT(block.size() <= ViewConstantsStride,
                  "BindlessRegistry::WriteViewConstants: block is {} bytes, exceeds stride {}",
                  block.size(), ViewConstantsStride);

        // Write only the current view slot's region; it is rewritten every Execute and each
        // view slot is distinct within the frame, so other regions need no flush.
        const u64 offset = static_cast<u64>(GetCurrentViewConstantsIndex()) * ViewConstantsStride;
        auto* base = static_cast<u8*>(m_ViewConstantsBuffer->GetMappedData());
        std::memcpy(base + offset, block.data(), block.size());
    }

    u32 BindlessRegistry::GetCurrentViewConstantsIndex() const
    {
        return m_Context.GetCurrentFrameInFlight() * MaxViewsPerFrame + m_ViewSlot;
    }

    void BindlessRegistry::WriteLights(std::span<const std::byte> lights)
    {
        VE_ASSERT(lights.size() <= static_cast<usize>(MaxLights) * LightStride,
                  "BindlessRegistry::WriteLights: {} bytes exceeds the {}-light region ({} bytes)",
                  lights.size(), MaxLights, static_cast<usize>(MaxLights) * LightStride);

        // Write only the current view slot's region; it is rewritten every Execute and each view
        // slot is distinct within the frame, so other regions need no flush.
        const u64 offset = static_cast<u64>(GetCurrentLightBase()) * LightStride;
        auto* base = static_cast<u8*>(m_LightBuffer->GetMappedData());
        std::memcpy(base + offset, lights.data(), lights.size());
    }

    u32 BindlessRegistry::GetCurrentLightBase() const
    {
        return GetCurrentViewConstantsIndex() * MaxLights;
    }

    void BindlessRegistry::WriteAreaVertices(std::span<const std::byte> vertices)
    {
        VE_ASSERT(
            vertices.size() <= static_cast<usize>(MaxAreaVertices) * AreaVertexStride,
            "BindlessRegistry::WriteAreaVertices: {} bytes exceeds the {}-vertex region ({} bytes)",
            vertices.size(), MaxAreaVertices,
            static_cast<usize>(MaxAreaVertices) * AreaVertexStride);

        const u64 offset = static_cast<u64>(GetCurrentAreaVertexBase()) * AreaVertexStride;
        auto* base = static_cast<u8*>(m_AreaVertexBuffer->GetMappedData());
        std::memcpy(base + offset, vertices.data(), vertices.size());
    }

    u32 BindlessRegistry::GetCurrentAreaVertexBase() const
    {
        return GetCurrentViewConstantsIndex() * MaxAreaVertices;
    }

    void BindlessRegistry::OnFrameAcquired(u32 frameInFlight)
    {
        m_Textures.OnFrameAcquired(frameInFlight);
        m_Volumes.OnFrameAcquired(frameInFlight);
        m_Cubes.OnFrameAcquired(frameInFlight);
        m_Samplers.OnFrameAcquired(frameInFlight);
        m_StorageImages.OnFrameAcquired(frameInFlight);
        m_StorageBuffers.OnFrameAcquired(frameInFlight);
        m_Materials.OnFrameAcquired(frameInFlight);

        // Reset the per-frame view slot: the first Viewport::Render this frame takes slot 0.
        m_ViewsThisFrame = 0;
        m_ViewSlot = 0;

        // Flush still-dirty materials into the region just made current — the fence
        // was waited before this call, so the prior GPU use has completed.
        for (u32 i = 0; i < MaxMaterials; i++)
        {
            MaterialEntry& entry = m_MaterialEntries[i];
            if (entry.DirtyFrames == 0)
            {
                continue;
            }
            WriteMaterialRegion(i, frameInFlight);
            entry.DirtyFrames--;
        }
    }
}
