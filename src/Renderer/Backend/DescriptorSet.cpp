#include <Veng/Renderer/Backend/DescriptorSet.h>

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

        m_DescriptorSet = Context::Instance().GetVkDevice().allocateDescriptorSets(allocateInfo)[0];
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
        vector<vector<vk::DescriptorImageInfo>*> imageInfos;
        vector<vector<vk::DescriptorBufferInfo>*> bufferInfos;

        imageInfos.reserve(info.Writes.size());
        bufferInfos.reserve(info.Writes.size());

        for (const auto& write : info.Writes)
        {
            auto writeImageInfos = new vector<vk::DescriptorImageInfo>();
            imageInfos.push_back(writeImageInfos);
            auto writeBufferInfos = new vector<vk::DescriptorBufferInfo>();
            bufferInfos.push_back(writeBufferInfos);

            switch (write.Type)
            {
            case vk::DescriptorType::eCombinedImageSampler:
                {
                    const auto& data = std::get<vector<DescriptorImageInfo>>(write.Data);
                    writeImageInfos->reserve(data.size());

                    auto binding = write.Binding;
                    for (const auto& image : data)
                    {
                        writeImageInfos->push_back({
                            .imageView = image.ImageView->GetVkImageView(),
                            .sampler = image.Sampler->GetVkSampler(),
                            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                        });

                        m_BoundResources[binding++] = CreateRef<DescriptorSetImageBinding>(
                            image.ImageView, image.Sampler);
                    }

                    writes.push_back({
                        .dstSet = m_DescriptorSet,
                        .dstBinding = write.Binding,
                        .dstArrayElement = write.ArrayElement,
                        .descriptorType = write.Type,
                        .descriptorCount = static_cast<u32>(writeImageInfos->size()),
                        .pImageInfo = writeImageInfos->data()
                    });


                    break;
                }
            case vk::DescriptorType::eUniformBuffer:
                {
                    const auto& data = std::get<vector<DescriptorBufferInfo>>(write.Data);
                    writeBufferInfos->reserve(data.size());
                    auto binding = write.Binding;
                    for (const auto& buffer : data)
                    {
                        writeBufferInfos->push_back({
                            .buffer = buffer.Buffer->GetVkBuffer(),
                            .offset = buffer.Offset,
                            .range = buffer.Range
                        });
                        m_BoundResources[binding++] = buffer.Buffer;
                    }

                    writes.push_back({
                        .dstSet = m_DescriptorSet,
                        .dstBinding = write.Binding,
                        .dstArrayElement = static_cast<u32>(write.ArrayElement),
                        .descriptorType = write.Type,
                        .descriptorCount = static_cast<u32>(writeBufferInfos->size()),
                        .pBufferInfo = writeBufferInfos->data()
                    });
                    break;
                }
            default: break;
            }
        }

        Context::Instance().GetVkDevice().updateDescriptorSets(writes, {});

        for (auto imageInfo : imageInfos)
        {
            delete imageInfo;
        }

        for (auto bufferInfo : bufferInfos)
        {
            delete bufferInfo;
        }
    }
}
