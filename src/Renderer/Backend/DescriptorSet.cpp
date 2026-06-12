#include <Veng/Renderer/Backend/DescriptorSet.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>


namespace Veng::Renderer
{
    DescriptorSet::DescriptorSet(const DescriptorSetInfo& info) : m_Name(info.Name), m_Layout(info.Layout)
    {
        vector<vk::DescriptorSetLayout> layouts = {info.Layout->GetVkDescriptorSetLayout()};

        const vk::DescriptorSetAllocateInfo allocateInfo = {
            .descriptorPool = Context::Instance().GetDescriptorPool().GetVkDescriptorPool(),
            .descriptorSetCount = static_cast<u32>(layouts.size()),
            .pSetLayouts = layouts.data()
        };

        m_DescriptorSet = Context::Instance().GetVkDevice().allocateDescriptorSets(allocateInfo).value[0];

        const auto& bindings = m_Layout->GetBindings();
        for (u32 i = 0; i < bindings.size(); i++)
        {
            VE_ASSERT(bindings[i].binding == i,
                      "DescriptorSetLayout '{}' has non-dense binding numbers; binding {} is at index {}",
                      m_Layout->GetName(), bindings[i].binding, i);
        }

        m_BoundResources.resize(m_Layout->GetBindingCount(), nullptr);

        DebugMarkers::MarkDescriptorSet(m_DescriptorSet, m_Name);
    }

    DescriptorSet::~DescriptorSet()
    {
        Context::Instance().GetVkDevice().freeDescriptorSets(
            Context::Instance().GetDescriptorPool().GetVkDescriptorPool(), m_DescriptorSet);
    }

    void DescriptorSet::UpdateDescriptorSet(const DescriptorSetUpdateInfo& info)
    {
        vector<vk::WriteDescriptorSet> writes;

        // Reserved up front so the pointers stored in `writes` stay valid.
        vector<vector<vk::DescriptorImageInfo>> imageInfos;
        vector<vector<vk::DescriptorBufferInfo>> bufferInfos;
        imageInfos.reserve(info.Writes.size());
        bufferInfos.reserve(info.Writes.size());

        for (const auto& write : info.Writes)
        {
            switch (write.Type)
            {
            case vk::DescriptorType::eCombinedImageSampler:
                {
                    const auto& data = std::get<vector<DescriptorImageInfo>>(write.Data);

                    auto& writeImageInfos = imageInfos.emplace_back();
                    writeImageInfos.reserve(data.size());

                    auto binding = write.Binding;
                    for (const auto& image : data)
                    {
                        writeImageInfos.push_back({
                            .sampler = image.Sampler->GetVkSampler(),
                            .imageView = image.ImageView->GetVkImageView(),
                            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                        });

                        m_BoundResources[binding++] = CreateRef<DescriptorSetImageBinding>(
                            image.ImageView, image.Sampler);
                    }

                    writes.push_back({
                        .dstSet = m_DescriptorSet,
                        .dstBinding = write.Binding,
                        .dstArrayElement = write.ArrayElement,
                        .descriptorCount = static_cast<u32>(writeImageInfos.size()),
                        .descriptorType = write.Type,
                        .pImageInfo = writeImageInfos.data()
                    });

                    break;
                }
            case vk::DescriptorType::eUniformBuffer:
                {
                    const auto& data = std::get<vector<DescriptorBufferInfo>>(write.Data);

                    auto& writeBufferInfos = bufferInfos.emplace_back();
                    writeBufferInfos.reserve(data.size());

                    auto binding = write.Binding;
                    for (const auto& buffer : data)
                    {
                        writeBufferInfos.push_back({
                            .buffer = buffer.Buffer->GetVkBuffer(),
                            .offset = buffer.Offset,
                            .range = buffer.Range
                        });
                        m_BoundResources[binding++] = buffer.Buffer;
                    }

                    writes.push_back({
                        .dstSet = m_DescriptorSet,
                        .dstBinding = write.Binding,
                        .dstArrayElement = write.ArrayElement,
                        .descriptorCount = static_cast<u32>(writeBufferInfos.size()),
                        .descriptorType = write.Type,
                        .pBufferInfo = writeBufferInfos.data()
                    });
                    break;
                }
            default:
                VE_ASSERT(false, "DescriptorSet '{}': unsupported descriptor type {}", m_Name,
                          string_VkDescriptorType(static_cast<VkDescriptorType>(write.Type)));
                break;
            }
        }

        Context::Instance().GetVkDevice().updateDescriptorSets(writes, {});
    }
}
