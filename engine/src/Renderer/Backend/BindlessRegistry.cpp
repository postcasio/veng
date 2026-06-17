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
        VE_ASSERT(!Free.empty(), "BindlessRegistry: {} array exhausted ({} slots)", what, Slots.size());

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
        m_Layout = DescriptorSetLayout::Create(context, {
            .Name = "Bindless Set 0 Layout",
            .Bindings = {
                {.Binding = TextureBinding, .Type = DescriptorType::SampledImage, .Count = MaxTextures, .Stages = ShaderStage::All, .Bindless = true},
                {.Binding = SamplerBinding, .Type = DescriptorType::Sampler, .Count = MaxSamplers, .Stages = ShaderStage::All, .Bindless = true},
                {.Binding = StorageImageBinding, .Type = DescriptorType::StorageImage, .Count = MaxStorageImages, .Stages = ShaderStage::All, .Bindless = true},
                // The MaterialData array is a single storage buffer (the array
                // lives *inside* it, indexed by materialIndex), not an arrayed
                // binding — written once below, so no Bindless flag.
                {.Binding = MaterialBinding, .Type = DescriptorType::StorageBuffer, .Count = 1, .Stages = ShaderStage::All},
                // The authored-param buffer: a single ByteAddressBuffer on the
                // shader side, byte-addressed at materialIndex * MaterialParamStride.
                {.Binding = MaterialParamBinding, .Type = DescriptorType::StorageBuffer, .Count = 1, .Stages = ShaderStage::All},
            },
        });

        m_Set = DescriptorSet::Create(context, {
            .Name = "Bindless Set 0",
            .Layout = m_Layout,
        });

        m_MaterialBuffer = Buffer::Create(context, {
            .Name = "Bindless MaterialData",
            .Size = static_cast<u64>(MaxMaterials) * sizeof(MaterialData),
            .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
        });
        m_Set->Write(MaterialBinding, m_MaterialBuffer);

        m_MaterialParamBuffer = Buffer::Create(context, {
            .Name = "Bindless MaterialParams",
            .Size = static_cast<u64>(MaxMaterials) * MaterialParamStride,
            .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
        });
        m_Set->Write(MaterialParamBinding, m_MaterialParamBuffer);

        const u32 framesInFlight = context.GetMaxFramesInFlight();
        m_Textures.Init(MaxTextures, framesInFlight);
        m_Samplers.Init(MaxSamplers, framesInFlight);
        m_StorageImages.Init(MaxStorageImages, framesInFlight);
        m_Materials.Init(MaxMaterials, framesInFlight);
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

    MaterialHandle BindlessRegistry::RegisterMaterial(
        std::span<const std::byte> engine, std::span<const std::byte> authored)
    {
        const u32 index = m_Materials.Allocate(Ref<void>{}, "material");
        UpdateMaterial(MaterialHandle{index}, engine, authored);
        return MaterialHandle{index};
    }

    void BindlessRegistry::UpdateMaterial(MaterialHandle handle,
        std::span<const std::byte> engine, std::span<const std::byte> authored) const
    {
        VE_ASSERT(handle.IsValid(), "BindlessRegistry::UpdateMaterial: invalid handle");
        VE_ASSERT(engine.size() == sizeof(MaterialData),
                  "BindlessRegistry::UpdateMaterial: engine block is {} bytes, expected {}",
                  engine.size(), sizeof(MaterialData));
        VE_ASSERT(authored.size() <= MaterialParamStride,
                  "BindlessRegistry::UpdateMaterial: authored block is {} bytes, exceeds stride {}",
                  authored.size(), MaterialParamStride);

        const std::span<const u8> engineBytes(
            reinterpret_cast<const u8*>(engine.data()), engine.size());
        m_MaterialBuffer->UploadSync(
            engineBytes, static_cast<u64>(handle.Index) * sizeof(MaterialData));

        if (!authored.empty())
        {
            const std::span<const u8> authoredBytes(
                reinterpret_cast<const u8*>(authored.data()), authored.size());
            m_MaterialParamBuffer->UploadSync(
                authoredBytes, static_cast<u64>(handle.Index) * MaterialParamStride);
        }
    }

    void BindlessRegistry::Release(TextureHandle handle)
    {
        if (!handle.IsValid()) return;
        m_Textures.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(SamplerHandle handle)
    {
        if (!handle.IsValid()) return;
        m_Samplers.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(StorageImageHandle handle)
    {
        if (!handle.IsValid()) return;
        m_StorageImages.ReleaseDeferred(handle.Index, m_Context.GetCurrentFrameInFlight());
    }

    void BindlessRegistry::Release(MaterialHandle handle)
    {
        if (!handle.IsValid()) return;
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

    void BindlessRegistry::OnFrameAcquired(u32 frameInFlight)
    {
        m_Textures.OnFrameAcquired(frameInFlight);
        m_Samplers.OnFrameAcquired(frameInFlight);
        m_StorageImages.OnFrameAcquired(frameInFlight);
        m_Materials.OnFrameAcquired(frameInFlight);
    }
}
