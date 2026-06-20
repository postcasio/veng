#include <Veng/Renderer/BindlessRegistry.h>

#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
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
                    },
            });

        m_Set = DescriptorSet::Create(context, {
                                                   .Name = "Bindless Set 0",
                                                   .Layout = m_Layout,
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

        // Ring-buffered by framesInFlight; each frame rewrites its own region every Execute.
        m_ViewConstantsBuffer = Buffer::Create(
            context, {
                         .Name = "Bindless ViewConstants",
                         .Size = static_cast<u64>(m_FramesInFlight) * ViewConstantsStride,
                         .Usage = BufferUsage::Storage,
                         .HostMapped = true,
                     });
        m_Set->Write(ViewConstantsBinding, m_ViewConstantsBuffer);

        // Ring-buffered by framesInFlight; each frame rewrites its own region every Execute.
        m_LightBuffer = Buffer::Create(
            context, {
                         .Name = "Bindless Lights",
                         .Size = static_cast<u64>(m_FramesInFlight) * MaxLights * LightStride,
                         .Usage = BufferUsage::Storage,
                         .HostMapped = true,
                     });
        m_Set->Write(LightBinding, m_LightBuffer);

        m_Textures.Init(MaxTextures, m_FramesInFlight);
        m_Samplers.Init(MaxSamplers, m_FramesInFlight);
        m_StorageImages.Init(MaxStorageImages, m_FramesInFlight);
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

    TextureHandle BindlessRegistry::Register(const Ref<ImageView>& sampled)
    {
        const u32 index = m_Textures.Allocate(sampled, "texture");
        WriteTexture(index, sampled);
        return TextureHandle{index};
    }

    SamplerHandle BindlessRegistry::Register(const Ref<Sampler>& sampler)
    {
        const u32 index = m_Samplers.Allocate(sampler, "sampler");
        WriteSampler(index, sampler);
        return SamplerHandle{index};
    }

    StorageImageHandle BindlessRegistry::RegisterStorage(const Ref<ImageView>& storage)
    {
        const u32 index = m_StorageImages.Allocate(storage, "storage image");
        WriteStorageImage(index, storage);
        return StorageImageHandle{index};
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
            return;

        const u64 regionBase = static_cast<u64>(frameInFlight) * MaxMaterials * MaterialParamStride;
        const u64 offset = regionBase + static_cast<u64>(materialIndex) * MaterialParamStride;
        auto* base = static_cast<u8*>(m_MaterialParamBuffer->GetMappedData());
        std::memcpy(base + offset, entry.Block.data(), entry.Block.size());
    }

    void BindlessRegistry::Release(TextureHandle handle)
    {
        if (!handle.IsValid())
            return;
        m_Textures.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(SamplerHandle handle)
    {
        if (!handle.IsValid())
            return;
        m_Samplers.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(StorageImageHandle handle)
    {
        if (!handle.IsValid())
            return;
        m_StorageImages.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(MaterialHandle handle)
    {
        if (!handle.IsValid())
            return;
        m_Materials.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Bind(CommandBuffer& cmd, PipelineBindPoint bindPoint) const
    {
        cmd.BindDescriptorSets({
            .Sets = {m_Set},
            .FirstSet = 0,
            .PipelineBindPoint = bindPoint,
        });
    }

    u32 BindlessRegistry::GetCurrentFrameBase() const
    {
        return m_Context.GetCurrentFrameInFlight() * MaxMaterials;
    }

    void BindlessRegistry::WriteViewConstants(std::span<const std::byte> block)
    {
        VE_ASSERT(block.size() <= ViewConstantsStride,
                  "BindlessRegistry::WriteViewConstants: block is {} bytes, exceeds stride {}",
                  block.size(), ViewConstantsStride);

        // Write only the current frame's region; it is rewritten every Execute so
        // other regions need no flush.
        const u64 offset =
            static_cast<u64>(m_Context.GetCurrentFrameInFlight()) * ViewConstantsStride;
        auto* base = static_cast<u8*>(m_ViewConstantsBuffer->GetMappedData());
        std::memcpy(base + offset, block.data(), block.size());
    }

    u32 BindlessRegistry::GetCurrentViewConstantsIndex() const
    {
        return m_Context.GetCurrentFrameInFlight();
    }

    void BindlessRegistry::WriteLights(std::span<const std::byte> lights)
    {
        VE_ASSERT(lights.size() <= static_cast<usize>(MaxLights) * LightStride,
                  "BindlessRegistry::WriteLights: {} bytes exceeds the {}-light region ({} bytes)",
                  lights.size(), MaxLights, static_cast<usize>(MaxLights) * LightStride);

        // Write only the current frame's region; it is rewritten every Execute so
        // other regions need no flush.
        const u64 offset =
            static_cast<u64>(m_Context.GetCurrentFrameInFlight()) * MaxLights * LightStride;
        auto* base = static_cast<u8*>(m_LightBuffer->GetMappedData());
        std::memcpy(base + offset, lights.data(), lights.size());
    }

    u32 BindlessRegistry::GetCurrentLightBase() const
    {
        return m_Context.GetCurrentFrameInFlight() * MaxLights;
    }

    void BindlessRegistry::OnFrameAcquired(u32 frameInFlight)
    {
        m_Textures.OnFrameAcquired(frameInFlight);
        m_Samplers.OnFrameAcquired(frameInFlight);
        m_StorageImages.OnFrameAcquired(frameInFlight);
        m_Materials.OnFrameAcquired(frameInFlight);

        // Flush still-dirty materials into the region just made current — the fence
        // was waited before this call, so the prior GPU use has completed.
        for (u32 i = 0; i < MaxMaterials; i++)
        {
            MaterialEntry& entry = m_MaterialEntries[i];
            if (entry.DirtyFrames == 0)
                continue;
            WriteMaterialRegion(i, frameInFlight);
            entry.DirtyFrames--;
        }
    }
}
