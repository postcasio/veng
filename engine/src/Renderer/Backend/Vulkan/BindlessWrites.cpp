// The Vulkan half of the bindless registry: each slot write is one descriptor update into the
// registry's update-after-bind sets, and the frame-slot queries read the context's frame fences.

#include <Veng/Renderer/BindlessRegistry.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Sampler.h>
#include "../../BindlessBackend.h"
#include "Natives.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    u32 BindlessBackend::GetReleaseSlot(const Context& context)
    {
        return context.GetNative().GetReleaseSlot();
    }

    bool BindlessBackend::IsSlotWritable(const Context& context, const u32 slot)
    {
        return context.GetNative().IsSlotWritable(slot);
    }

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

    void BindlessRegistry::WriteNormalVariance(u32 index, const Ref<ImageView>& view) const
    {
        const vk::DescriptorImageInfo imageInfo{
            .imageView = GetVkImageView(*view),
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = GetVkDescriptorSet(*m_Set),
            .dstBinding = NormalVarianceBinding,
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
}
