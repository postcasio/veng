
#include "material.h"
#include "engine.h"

Material::Material(MaterialDefinition materialDefinition) : materialDefinition(materialDefinition)
{
    // Load textures
    if (materialDefinition.diffuseMapPath.empty())
    {
        diffuseMap = engine()->textureCache->getTexture("textures/null.png");
    }
    else
    {
        diffuseMap = engine()->textureCache->getTexture(materialDefinition.diffuseMapPath);
    }

    if (materialDefinition.normalMapPath.empty())
    {
        normalMap = engine()->textureCache->getTexture("textures/null.png");
    }
    else
    {
        normalMap = engine()->textureCache->getTexture(materialDefinition.normalMapPath, VK_FORMAT_R8G8B8A8_UNORM);
    }

    if (materialDefinition.displacementMapPath.empty())
    {
        displacementMap = engine()->textureCache->getTexture("textures/null.png");
    }
    else
    {
        displacementMap = engine()->textureCache->getTexture(materialDefinition.displacementMapPath);
    }

    if (materialDefinition.occlusionMapPath.empty())
    {
        occlusionMap = engine()->textureCache->getTexture("textures/null.png");
    }
    else
    {
        occlusionMap = engine()->textureCache->getTexture(materialDefinition.occlusionMapPath);
    }

    if (materialDefinition.roughnessMapPath.empty())
    {
        roughnessMap = engine()->textureCache->getTexture("textures/null.png");
    }
    else
    {
        roughnessMap = engine()->textureCache->getTexture(materialDefinition.roughnessMapPath);
    }

    // Create descriptor set
    descriptorSet = renderer()->descriptorPool->createDescriptorSet(*renderer()->materialDescriptorSetLayout);

    updateDescriptorSet();
}

Material::~Material()
{
    descriptorSet.reset();
}

bool Material::matchesDefinition(MaterialDefinition materialDefinition)
{
    return !this->materialDefinition.diffuseMapPath.compare(materialDefinition.diffuseMapPath) &&
           !this->materialDefinition.normalMapPath.compare(materialDefinition.normalMapPath) &&
           !this->materialDefinition.displacementMapPath.compare(materialDefinition.displacementMapPath) &&
           !this->materialDefinition.occlusionMapPath.compare(materialDefinition.occlusionMapPath) &&
           !this->materialDefinition.roughnessMapPath.compare(materialDefinition.roughnessMapPath);
}

void Material::updateDescriptorSet()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorImageInfo diffuseImageInfo{};
        diffuseImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        diffuseImageInfo.imageView = diffuseMap->imageView->view;
        diffuseImageInfo.sampler = diffuseMap->sampler->sampler;

        VkDescriptorImageInfo normalImageInfo{};
        normalImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        normalImageInfo.imageView = normalMap->imageView->view;
        normalImageInfo.sampler = normalMap->sampler->sampler;

        VkDescriptorImageInfo occlusionImageInfo{};
        occlusionImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        occlusionImageInfo.imageView = occlusionMap->imageView->view;
        occlusionImageInfo.sampler = occlusionMap->sampler->sampler;

        VkDescriptorImageInfo roughnessImageInfo{};
        roughnessImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        roughnessImageInfo.imageView = roughnessMap->imageView->view;
        roughnessImageInfo.sampler = roughnessMap->sampler->sampler;

        std::array<VkWriteDescriptorSet, 4> descriptorWrites{};

        descriptorWrites[0]
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = descriptorSet->sets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pImageInfo = &diffuseImageInfo;

        descriptorWrites[1]
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = descriptorSet->sets[i];
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &normalImageInfo;

        descriptorWrites[2]
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[2].dstSet = descriptorSet->sets[i];
        descriptorWrites[2].dstBinding = 2;
        descriptorWrites[2].dstArrayElement = 0;
        descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[2].descriptorCount = 1;
        descriptorWrites[2].pImageInfo = &occlusionImageInfo;

        descriptorWrites[3]
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[3].dstSet = descriptorSet->sets[i];
        descriptorWrites[3].dstBinding = 3;
        descriptorWrites[3].dstArrayElement = 0;
        descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[3].descriptorCount = 1;
        descriptorWrites[3].pImageInfo = &roughnessImageInfo;

        vkUpdateDescriptorSets(renderer()->device->device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    }
}